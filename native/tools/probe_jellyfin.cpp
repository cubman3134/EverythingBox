// Headless check of MULTIPLE JELLYFIN SERVERS MERGED INTO ONE LIBRARY (issue #160, increment 1) — the
// server-qualified item id, the migration that moves rows onto it, the connected-server store, and the
// union across servers with its failure isolation.
//
// The property that matters more than everything else here, and is pinned from BOTH sides:
//
//     AN ID FROM ONE SERVER MUST NEVER RESOLVE AGAINST ANOTHER, AND A MIGRATION THAT SILENTLY LOSES A
//     USER'S DATA IS FAR WORSE THAN ONE THAT LEAVES A FEW ROWS BEHIND ON OLD KEYS.
//
// So every section that proves an unmappable reference is LEFT ALONE is paired with one that proves the
// mappable one still moves — because a migration biased to do nothing passes every "did not lose anything"
// test by being a no-op, and a no-op is the whole feature failing silently.
// native/tools/jellyfin-mutants.json mutates in both directions for exactly that reason.
//
// Every key shape below is recomputed here from first principles (the digest, the truncation, the group
// path) rather than by calling the migration's own helpers, so a drift between this file and the stores it
// mirrors shows up as a failing check instead of as a passing tautology.
//
// NO NETWORK, WITH ONE LOOPBACK EXCEPTION. Everything under test is pure or is a QSettings store; the socket
// half (JellyfinClient) is driven live against fixture servers. Section 20 (#83, Quick Connect) is the
// exception: the REAL polling session runs against FakeJellyfin on 127.0.0.1, because "cancel stops polling"
// is a claim about requests a server stops receiving. No real server is contacted.
//
// NO CREDENTIAL IS EVER PRINTED. The fixture token below is compared, hashed and searched for — never
// written to stdout or stderr, including inside a failing CHECK, which is why the credential sections
// assert on booleans computed beforehand rather than on expressions naming the token.
//
// Prints JELLYFIN-OK on success; any failure prints JELLYFIN-FAIL <cond> and exits non-zero.
#include "Jellyfin.h"
#include "JellyfinClient.h"          // #160 inc 2: the REAL fan-out, so a filtered browse can be COUNTED
#include "JellyfinMigrate.h"
#include "JellyfinQuickConnect.h"   // #83 section 20: the Quick Connect session, driven over a socket
#include "JellyfinServerStore.h"
#include "RecentStore.h"      // #83: the byte scan below writes a REAL recents row and reads the ini back

#include "AppBrand.h"
#include "AppPaths.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDeadlineTimer>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QHostAddress>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QTcpServer>
#include <QTcpSocket>
#include <QVector>
#include <cstdio>
#include <functional>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "JELLYFIN-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

// Two real-shaped server ids (32 hex digits) and one item id shared by BOTH of them — which is the whole
// point of section 1: the same raw id on two servers must produce two different stored references.
static const char* kSrvA = "0123456789abcdef0123456789abcdef";
static const char* kSrvB = "fedcba9876543210fedcba9876543210";
static const char* kItem = "aaaaaaaabbbbccccddddeeeeeeeeeeee";

// ---------------------------------------------------------------------------------------------------------
// The key shapes, recomputed independently. See the header.
// ---------------------------------------------------------------------------------------------------------
static QString md5Full(const QString& s)
{
    return QString::fromLatin1(QCryptographicHash::hash(s.toUtf8(), QCryptographicHash::Md5).toHex());
}
static QString md5Ten(const QString& s) { return md5Full(s).left(10); }
static QString sha1Full(const QString& s)
{
    return QString::fromLatin1(QCryptographicHash::hash(s.toUtf8(), QCryptographicHash::Sha1).toHex());
}
static QString resumeGroup(const QString& id) { return QStringLiteral("resume/") + md5Ten(id); }
static QString marksKey(const QString& profile, const QString& id)
{
    return QStringLiteral("marks/") + profile + QStringLiteral("/items/") + md5Full(id);
}
static QString playStatsKey(const QString& profile, const QString& device, const QString& id)
{
    return QStringLiteral("playstats/") + profile + QLatin1Char('/') + device
         + QLatin1Char('/') + sha1Full(id);
}
static QString playStatsLegacyKey(const QString& profile, const QString& id)
{
    return QStringLiteral("playstats/") + profile + QLatin1Char('/') + sha1Full(id);
}

static QString tmpDir()
{
    return QDir::tempPath() + QStringLiteral("/eb-probe-jellyfin");
}

// =========================================================================================================
// A FAKE JELLYFIN FOR QUICK CONNECT (issue #83, section 20). HTTP/1.1, one request per connection, no
// keep-alive — the SeerrStub shape from probe_requests. It answers the four Quick Connect calls and nothing
// else, records every request it was sent, and has one knob per server behaviour the flow has to survive.
// It listens on 127.0.0.1 only. No real server is contacted anywhere in this probe.
// =========================================================================================================
class FakeJellyfin : public QTcpServer
{
public:
    struct Seen { QString method; QString path; QByteArray query; bool hasAuthHeader = false; QByteArray body; };
    QVector<Seen> seen;

    int        enabledStatus = 200;
    QByteArray enabledBody   = "true";
    bool       initiateGetOnly = false;   // 10.8: POST answers 405, GET answers the result
    int        initiateStatus  = 200;
    int        pendingPolls    = 2;       // Connect answers Authenticated:false this many times first
    int        connectStatus   = 200;     // anything but 200 is sent as-is (404 = expired secret)
    QByteArray authBody;                  // /Users/AuthenticateWithQuickConnect's AuthenticationResult

    // #160 increment 2: the two BROWSE answers, so the real JellyfinClient fan-out can be driven against
    // two of these at once. Matched by path SUFFIX, so the fixture does not have to know which user id the
    // store was seeded with. An empty body here means "this server answers 404", which is how a leg that
    // contributes nothing is staged.
    QByteArray viewsBody;                 // /Users/<uid>/Views
    QByteArray resumeBody;                // /Users/<uid>/Items/Resume

    static constexpr const char* kSecret = "5ec2e7a5ec2e7a5ec2e7a5ec2e7a5ec2";
    static constexpr const char* kCode   = "482915";

    explicit FakeJellyfin(QObject* parent = nullptr) : QTcpServer(parent) {}

    int countOf(const QString& method, const QString& path) const
    {
        int n = 0;
        for (const Seen& s : seen) if (s.method == method && s.path == path) ++n;
        return n;
    }
    const Seen* lastOf(const QString& method, const QString& path) const
    {
        for (int i = int(seen.size()) - 1; i >= 0; --i)
            if (seen[i].method == method && seen[i].path == path) return &seen[i];
        return nullptr;
    }
    QString root() const { return QStringLiteral("http://127.0.0.1:%1").arg(serverPort()); }

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
            int wantBody = 0;
            bool auth = false;
            for (int i = 1; i < lines.size(); ++i)
            {
                const QByteArray l = lines.at(i).trimmed().toLower();   // Qt 6 lower-cases header names
                if (l.startsWith("content-length:")) wantBody = l.mid(15).trimmed().toInt();
                if (l.startsWith("authorization: mediabrowser ")) auth = true;
            }
            const QByteArray body = buf.mid(headEnd + 4);
            if (body.size() < wantBody) return;
            const QByteArray target = reqLine.value(1);
            const int q = target.indexOf('?');
            Seen s;
            s.method = QString::fromLatin1(reqLine.value(0));
            s.path   = QString::fromLatin1(q < 0 ? target : target.left(q));
            s.query  = q < 0 ? QByteArray() : target.mid(q + 1);
            s.hasAuthHeader = auth;
            s.body   = body.left(wantBody);
            seen.push_back(s);
            reply(sock, s);
        });
    }

private:
    void send(QTcpSocket* sock, int status, const QByteArray& body)
    {
        sock->write("HTTP/1.1 " + QByteArray::number(status) + (status < 400 ? " OK" : " ERR")
                    + "\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(body.size())
                    + "\r\nConnection: close\r\n\r\n" + body);
        sock->flush();
        sock->disconnectFromHost();
    }
    QByteArray result(bool authenticated) const
    {
        return QByteArray("{\"Authenticated\":") + (authenticated ? "true" : "false")
             + ",\"Secret\":\"" + kSecret + "\",\"Code\":\"" + kCode
             + "\",\"DeviceId\":\"d\",\"DeviceName\":\"n\",\"AppName\":\"a\",\"AppVersion\":\"1\"}";
    }
    void reply(QTcpSocket* sock, const Seen& s)
    {
        // #160 increment 2: the browse answers. First, so that a request this probe is counting is never
        // also matched by one of the Quick Connect arms below.
        if (s.path.endsWith(QLatin1String("/Views")))
        { send(sock, viewsBody.isEmpty() ? 404 : 200, viewsBody); return; }
        if (s.path.endsWith(QLatin1String("/Items/Resume")))
        { send(sock, resumeBody.isEmpty() ? 404 : 200, resumeBody); return; }
        if (s.path == QLatin1String("/QuickConnect/Enabled"))
        { send(sock, enabledStatus, enabledBody); return; }
        if (s.path == QLatin1String("/QuickConnect/Initiate"))
        {
            if (initiateGetOnly && s.method != QLatin1String("GET")) { send(sock, 405, ""); return; }
            if (!initiateGetOnly && s.method != QLatin1String("POST")) { send(sock, 405, ""); return; }
            if (initiateStatus != 200) { send(sock, initiateStatus, "\"Quick connect is disabled\""); return; }
            send(sock, 200, result(false));
            return;
        }
        if (s.path == QLatin1String("/QuickConnect/Connect"))
        {
            if (connectStatus != 200) { send(sock, connectStatus, "\"Unknown secret\""); return; }
            const bool known = s.query == QByteArray("secret=") + kSecret;
            if (!known) { send(sock, 404, "\"Unknown secret\""); return; }
            if (pendingPolls > 0) { --pendingPolls; send(sock, 200, result(false)); return; }
            send(sock, 200, result(true));
            return;
        }
        if (s.path == QLatin1String("/Users/AuthenticateWithQuickConnect") && s.method == QLatin1String("POST"))
        {
            const bool rightSecret = QJsonDocument::fromJson(s.body).object()
                                         .value(QStringLiteral("Secret")).toString() == QLatin1String(kSecret);
            if (!rightSecret) { send(sock, 400, ""); return; }
            send(sock, 200, authBody);
            return;
        }
        send(sock, 404, "");
    }
};

// Spin the event loop until `pred` or the deadline. Every wait here is for a socket or a timer.
template <typename Pred>
static bool waitFor(Pred pred, int ms = 8000)
{
    QDeadlineTimer dl(ms);
    while (!pred() && !dl.hasExpired())
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return pred();
}
// Spin for a fixed time regardless — for proving that something does NOT happen.
static void spinFor(int ms)
{
    QDeadlineTimer dl(ms);
    while (!dl.hasExpired()) QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QDir().mkpath(tmpDir());

    const QString legacyId = QStringLiteral("jf:") + QLatin1String(kItem);
    const QString qualA    = Jellyfin::qualify(QString::fromLatin1(kSrvA), QString::fromLatin1(kItem));
    const QString qualB    = Jellyfin::qualify(QString::fromLatin1(kSrvB), QString::fromLatin1(kItem));

    // =====================================================================================================
    // 1. THE ID: minting, reading, and the collision that must be unspellable
    // =====================================================================================================
    {
        CHECK(qualA == QStringLiteral("jf:0123456789abcdef0123456789abcdef:") + QLatin1String(kItem));

        // THE COLLISION #160 EXISTS TO PREVENT. One raw item id, two servers, two different stored keys.
        CHECK(!qualA.isEmpty());
        CHECK(!qualB.isEmpty());
        CHECK(qualA != qualB);
        // And they differ in the SERVER field specifically — not by luck of the item half.
        CHECK(Jellyfin::parse(qualA).itemId == Jellyfin::parse(qualB).itemId);
        CHECK(Jellyfin::parse(qualA).serverId != Jellyfin::parse(qualB).serverId);

        // A round trip is exact.
        const Jellyfin::Ref r = Jellyfin::parse(qualA);
        CHECK(r.ok);
        CHECK(r.serverId == QLatin1String(kSrvA));
        CHECK(r.itemId == QLatin1String(kItem));

        // An item id containing the separator survives: the item half is EVERYTHING after the second colon,
        // never a section() split — a split would silently truncate it into a different item's id.
        const QString odd = QStringLiteral("weird:id:with:colons");
        const QString qOdd = Jellyfin::qualify(QString::fromLatin1(kSrvA), odd);
        CHECK(Jellyfin::parse(qOdd).ok);
        CHECK(Jellyfin::parse(qOdd).itemId == odd);
        CHECK(Jellyfin::parse(qOdd).serverId == QLatin1String(kSrvA));

        // An unqualifiable reference is ABSENT, never half-formed.
        CHECK(Jellyfin::qualify(QString(), QString::fromLatin1(kItem)).isEmpty());
        CHECK(Jellyfin::qualify(QString::fromLatin1(kSrvA), QString()).isEmpty());
        CHECK(Jellyfin::qualify(QStringLiteral("not-a-server-id"), QString::fromLatin1(kItem)).isEmpty());

        // The server-id shape. Compact and dashed are ONE identity; anything else is not a server id.
        CHECK(Jellyfin::isServerId(QLatin1String(kSrvA)));
        CHECK(Jellyfin::isServerId(QStringLiteral("01234567-89ab-cdef-0123-456789abcdef")));
        CHECK(!Jellyfin::isServerId(QStringLiteral("0123456789abcdef0123456789abcde")));   // 31
        CHECK(!Jellyfin::isServerId(QStringLiteral("0123456789abcdef0123456789abcdeg")));  // not hex
        CHECK(!Jellyfin::isServerId(QString()));

        // NOTHING ELSE IN THE WORLD PARSES AS A QUALIFIED ID. This is the arm that keeps the migration and
        // the router off every other key family in the app.
        for (const QString& s : { QStringLiteral("C:/Films/Blade Runner.mkv"),
                                  QStringLiteral("https://example.com/x"),
                                  QStringLiteral("tt0083658"),
                                  QStringLiteral("jf"),
                                  QStringLiteral("jf:"),
                                  QStringLiteral("jf:notaserverid:item"),
                                  QString() })
            CHECK(!Jellyfin::isQualified(s));

        CHECK(Jellyfin::serverOf(qualA) == QLatin1String(kSrvA));
        CHECK(Jellyfin::serverOf(QStringLiteral("C:/Films/x.mkv")).isEmpty());
    }

    // =====================================================================================================
    // 2. THE LEGACY SHAPE — and the arm that matters most, which is everything it must REFUSE
    // =====================================================================================================
    {
        CHECK(Jellyfin::legacyItemId(legacyId) == QLatin1String(kItem));
        CHECK(Jellyfin::isLegacy(legacyId));

        // A QUALIFIED id is not a legacy id. Without this the migration would re-qualify its own output on
        // the second run and produce jf:<srv>:jf:<srv>:<item> — the exact corruption rule 4 forbids.
        CHECK(Jellyfin::legacyItemId(qualA).isEmpty());
        CHECK(!Jellyfin::isLegacy(qualA));

        // Everything that is not a Jellyfin reference at all.
        for (const QString& s : { QStringLiteral("C:/Films/Blade Runner.mkv"),
                                  QStringLiteral("tt0083658"),
                                  QStringLiteral("jf"),
                                  QStringLiteral("jf:"),
                                  QStringLiteral("jfx:abc"),
                                  QStringLiteral("jf:notaserverid:item"),
                                  QString() })
            CHECK(Jellyfin::legacyItemId(s).isEmpty());
    }

    // =====================================================================================================
    // 3. TRANSPORT SAFETY — the plain-HTTP question is asked, never assumed
    // =====================================================================================================
    {
        CHECK(Jellyfin::checkUrl(QStringLiteral("https://jf.example.com"), false)
              == Jellyfin::UrlVerdict::Ok);
        CHECK(Jellyfin::checkUrl(QStringLiteral("http://10.0.0.4:8096"), false)
              == Jellyfin::UrlVerdict::InsecureRefused);
        CHECK(Jellyfin::checkUrl(QStringLiteral("http://10.0.0.4:8096"), true)
              == Jellyfin::UrlVerdict::Ok);
        CHECK(Jellyfin::checkUrl(QStringLiteral("ftp://jf.example.com"), true)
              == Jellyfin::UrlVerdict::NotHttp);
        CHECK(Jellyfin::checkUrl(QStringLiteral("not a url"), true) == Jellyfin::UrlVerdict::Malformed);

        CHECK(Jellyfin::normalizeRoot(QStringLiteral("https://jf.example.com///"), false)
              == QStringLiteral("https://jf.example.com"));
        // A refused url has NO root. There is no fallback, because there is no other server this could mean.
        CHECK(Jellyfin::normalizeRoot(QStringLiteral("http://10.0.0.4:8096"), false).isEmpty());
    }

    // =====================================================================================================
    // 4. AUTH — the header, the body, and the two readers' "both halves or neither" rules
    // =====================================================================================================
    {
        const QString noTok = Jellyfin::authHeader(QStringLiteral("EverythingBox"), QStringLiteral("TV"),
                                                   QStringLiteral("dev-1"), QStringLiteral("1.2"),
                                                   QString());
        CHECK(noTok.startsWith(QStringLiteral("MediaBrowser ")));
        // The pre-sign-in form carries NO Token field at all — an empty Token="" would be a credential the
        // server has to reject rather than an absent one it never sees.
        CHECK(!noTok.contains(QStringLiteral("Token=")));

        const QString withTok = Jellyfin::authHeader(QStringLiteral("EverythingBox"), QStringLiteral("TV"),
                                                     QStringLiteral("dev-1"), QStringLiteral("1.2"),
                                                     QStringLiteral("FIXTURE-TOKEN-A"));
        // Asserted as a BOOLEAN computed here, so the token never reaches a CHECK's stringified expression.
        const bool headerCarriesToken = withTok.contains(QStringLiteral("Token=\"FIXTURE-TOKEN-A\""));
        CHECK(headerCarriesToken);

        const QJsonObject body = QJsonDocument::fromJson(
            Jellyfin::authenticateBody(QStringLiteral("ann"), QStringLiteral("s3cret"))).object();
        CHECK(body.value(QStringLiteral("Username")).toString() == QStringLiteral("ann"));
        const bool bodyCarriesPassword = body.value(QStringLiteral("Pw")).toString()
                                         == QStringLiteral("s3cret");
        CHECK(bodyCarriesPassword);

        // /System/Info/Public: `ok` is about the IDENTITY and nothing else.
        const Jellyfin::PublicInfo good = Jellyfin::readPublicInfo(
            QByteArray("{\"Id\":\"0123456789abcdef0123456789abcdef\",\"ServerName\":\"Attic\","
                       "\"Version\":\"10.10.3\"}"));
        CHECK(good.ok);
        CHECK(good.serverId == QLatin1String(kSrvA));
        CHECK(good.serverName == QStringLiteral("Attic"));

        // A name and a version but no usable Id cannot qualify a single row: refused, and the id is cleared
        // rather than left as a value a caller might use anyway.
        const Jellyfin::PublicInfo bad = Jellyfin::readPublicInfo(
            QByteArray("{\"ServerName\":\"Attic\",\"Version\":\"10.10.3\",\"Id\":\"nope\"}"));
        CHECK(!bad.ok);
        CHECK(bad.serverId.isEmpty());
        CHECK(!Jellyfin::readPublicInfo(QByteArray("<html>404</html>")).ok);

        // AuthenticateByName: BOTH halves or neither. A token with no user id would present itself as a
        // successful sign-in that can never list anything.
        const Jellyfin::AuthResult ar = Jellyfin::readAuthResult(
            QByteArray("{\"AccessToken\":\"FIXTURE-TOKEN-A\",\"User\":{\"Id\":\"u1\",\"Name\":\"ann\"}}"));
        CHECK(ar.ok);
        CHECK(ar.userId == QStringLiteral("u1"));
        CHECK(!Jellyfin::readAuthResult(QByteArray("{\"AccessToken\":\"T\"}")).ok);
        CHECK(!Jellyfin::readAuthResult(QByteArray("{\"User\":{\"Id\":\"u1\"}}")).ok);
    }

    // =====================================================================================================
    // 5. ITEMS — and the difference between "no items" and "not an item envelope"
    // =====================================================================================================
    {
        bool ok = false;
        const QVector<Jellyfin::RemoteItem> items = Jellyfin::readItems(
            QByteArray("{\"Items\":[{\"Id\":\"i1\",\"Name\":\"Alien\",\"Type\":\"Movie\","
                       "\"ProductionYear\":1979,\"UserData\":{\"Played\":true}},"
                       "{\"Id\":\"i2\",\"Name\":\"Aliens\",\"Type\":\"Movie\"}]}"), &ok);
        CHECK(ok);
        CHECK(items.size() == 2);
        CHECK(items.at(0).name == QStringLiteral("Alien"));
        CHECK(items.at(0).year == 1979);
        CHECK(items.at(0).played);
        CHECK(!items.at(1).played);

        // An EMPTY library is a successful answer.
        bool emptyOk = false;
        CHECK(Jellyfin::readItems(QByteArray("{\"Items\":[]}"), &emptyOk).isEmpty());
        CHECK(emptyOk);

        // A proxy's HTML page, a truncated body, or a JSON object with no Items member is NOT an empty
        // library — the union renders those differently, so the reader must not collapse them.
        for (const QByteArray& b : { QByteArray("<html>502</html>"), QByteArray("{\"Items\":"),
                                     QByteArray("{\"TotalRecordCount\":0}"), QByteArray() })
        {
            bool o = true;
            Jellyfin::readItems(b, &o);
            CHECK(!o);
        }

        // A row with no id can never be qualified, so it is not a row.
        bool idOk = false;
        CHECK(Jellyfin::readItems(QByteArray("{\"Items\":[{\"Name\":\"nameless\"}]}"), &idOk).isEmpty());
        CHECK(idOk);
    }

    // =====================================================================================================
    // 6. THE STREAM URL — a credential, minted at hand-off
    // =====================================================================================================
    {
        const QString u = Jellyfin::streamUrl(QStringLiteral("https://jf.example.com"),
                                              QString::fromLatin1(kItem),
                                              QStringLiteral("FIXTURE-TOKEN-A"));
        CHECK(u.startsWith(QStringLiteral("https://jf.example.com/Videos/")));
        CHECK(u.contains(QStringLiteral("static=true")));
        const bool urlCarriesToken = u.contains(QStringLiteral("api_key=FIXTURE-TOKEN-A"));
        CHECK(urlCarriesToken);            // asserted as a boolean; the token is never in a printed string
        CHECK(Jellyfin::streamUrl(QString(), QString::fromLatin1(kItem),
                                  QStringLiteral("t")).isEmpty());
        CHECK(Jellyfin::streamUrl(QStringLiteral("https://x"), QString(), QStringLiteral("t")).isEmpty());
    }

    // =====================================================================================================
    // 7. THE UNION — the merged library, and failure isolation as the absence of a special case
    // =====================================================================================================
    {
        Jellyfin::RemoteItem a1; a1.id = QStringLiteral("shared"); a1.name = QStringLiteral("Alien");
        Jellyfin::RemoteItem a2; a2.id = QStringLiteral("only-a"); a2.name = QStringLiteral("Aliens");
        Jellyfin::RemoteItem b1; b1.id = QStringLiteral("shared"); b1.name = QStringLiteral("Alien");

        Jellyfin::ServerReply A; A.serverId = QLatin1String(kSrvA); A.serverName = QStringLiteral("Attic");
        A.items = { a1, a2 };
        Jellyfin::ServerReply B; B.serverId = QLatin1String(kSrvB); B.serverName = QStringLiteral("Basement");
        B.items = { b1 };

        const QVector<Jellyfin::UnionItem> u = Jellyfin::unionOf({ A, B });
        CHECK(u.size() == 3);
        // Stable and total: servers in the order given, items in the order that server gave them.
        CHECK(u.at(0).title == QStringLiteral("Alien"));
        CHECK(u.at(1).title == QStringLiteral("Aliens"));
        CHECK(u.at(2).title == QStringLiteral("Alien"));
        // NO CROSS-SERVER DEDUPE (the issue decides this deliberately): the same film on two servers is two
        // rows, distinguished by the server name each carries.
        CHECK(u.at(0).serverName == QStringLiteral("Attic"));
        CHECK(u.at(2).serverName == QStringLiteral("Basement"));
        // And their ids DIFFER even though the servers minted the same raw id.
        CHECK(u.at(0).id != u.at(2).id);
        CHECK(Jellyfin::parse(u.at(0).id).ok && Jellyfin::parse(u.at(2).id).ok);

        // A TIMED-OUT SERVER CONTRIBUTES NOTHING AND BLOCKS NOTHING — the other server's rows are all there.
        Jellyfin::ServerReply slow = B; slow.outcome = Jellyfin::Outcome::TimedOut;
        const QVector<Jellyfin::UnionItem> u2 = Jellyfin::unionOf({ A, slow });
        CHECK(u2.size() == 2);
        for (const Jellyfin::UnionItem& it : u2) CHECK(it.serverName == QStringLiteral("Attic"));

        // A DISABLED server's rows are HIDDEN, and a FAILED one's are absent, by the same absence of a
        // special case.
        Jellyfin::ServerReply off = B; off.outcome = Jellyfin::Outcome::Disabled;
        CHECK(Jellyfin::unionOf({ A, off }).size() == 2);
        Jellyfin::ServerReply bad = B; bad.outcome = Jellyfin::Outcome::Failed;
        CHECK(Jellyfin::unionOf({ A, bad }).size() == 2);
        // Every server down: an empty shelf, not a crash and not a partial one.
        CHECK(Jellyfin::unionOf({ slow, off, bad }).isEmpty());

        // An unqualifiable row is DROPPED rather than emitted bare — a bare id is the corruption the whole
        // feature exists to prevent, and there is no safe half-measure.
        Jellyfin::ServerReply noId; noId.serverId = QStringLiteral("not-a-server-id");
        noId.serverName = QStringLiteral("Broken"); noId.items = { a1 };
        CHECK(Jellyfin::unionOf({ noId }).isEmpty());

        // The one line an absent server is allowed to log: it names the server and says nothing else.
        CHECK(Jellyfin::unavailableNote(A).isEmpty());               // it contributed: nothing to say
        const QString note = Jellyfin::unavailableNote(slow);
        CHECK(!note.isEmpty());
        CHECK(note.contains(QStringLiteral("Basement")));
        CHECK(!note.contains(QStringLiteral("http")));
        CHECK(!note.contains(QStringLiteral("Token")));
        CHECK(!note.contains(QStringLiteral("api_key")));
        CHECK(!Jellyfin::unavailableNote(off).isEmpty());
        CHECK(!Jellyfin::unavailableNote(bad).isEmpty());
    }

    // =====================================================================================================
    // 8. THE STORE — N servers, per-server enable, and a token that lives in exactly one place
    // =====================================================================================================
    const QString srvIni = tmpDir() + QStringLiteral("/servers.ini");
    QFile::remove(srvIni);
    {
        JellyfinServerStore::setIniPathForTesting(srvIni);
        CHECK(!JellyfinServerStore::hasServers());

        JellyfinServer a;
        a.id = QLatin1String(kSrvA); a.name = QStringLiteral("Attic");
        a.url = QStringLiteral("https://attic.example.com");
        a.userId = QStringLiteral("u1"); a.token = QStringLiteral("FIXTURE-TOKEN-A");
        CHECK(JellyfinServerStore::add(a));

        JellyfinServer b;
        b.id = QLatin1String(kSrvB); b.name = QStringLiteral("Basement");
        b.url = QStringLiteral("https://basement.example.com");
        b.userId = QStringLiteral("u2"); b.token = QStringLiteral("FIXTURE-TOKEN-B");
        CHECK(JellyfinServerStore::add(b));

        CHECK(JellyfinServerStore::list().size() == 2);
        CHECK(JellyfinServerStore::enabled().size() == 2);    // absent `enabled` means ENABLED
        CHECK(JellyfinServerStore::ids().contains(QLatin1String(kSrvA)));

        // A server with no readable identity is REFUSED, and stores nothing: a row we cannot qualify is
        // worse than no row, because it looks like it worked and then files everything under dead keys.
        JellyfinServer junk; junk.id = QStringLiteral("nope"); junk.name = QStringLiteral("Junk");
        CHECK(!JellyfinServerStore::add(junk));
        CHECK(JellyfinServerStore::list().size() == 2);

        // Re-adding an id already present UPDATES rather than duplicating (a friend re-shares, a url moves).
        JellyfinServer aMoved = a; aMoved.url = QStringLiteral("https://attic2.example.com");
        CHECK(JellyfinServerStore::add(aMoved));
        CHECK(JellyfinServerStore::list().size() == 2);
        JellyfinServer got;
        CHECK(JellyfinServerStore::get(QLatin1String(kSrvA), got));
        CHECK(got.url == QStringLiteral("https://attic2.example.com"));

        // PER-SERVER ENABLE hides without forgetting: the row and its sign-in are still there.
        JellyfinServerStore::setEnabled(QLatin1String(kSrvB), false);
        CHECK(JellyfinServerStore::list().size() == 2);
        CHECK(JellyfinServerStore::enabled().size() == 1);
        CHECK(JellyfinServerStore::enabled().first().id == QLatin1String(kSrvA));
        CHECK(JellyfinServerStore::get(QLatin1String(kSrvB), got));
        CHECK(!got.token.isEmpty());                  // still signed in — this is not a removal
        JellyfinServerStore::setEnabled(QLatin1String(kSrvB), true);
        CHECK(JellyfinServerStore::enabled().size() == 2);

        // THE TOKEN LIVES UNDER "jellyfin/" AND NOWHERE ELSE. probe_cloudmerge pins that this prefix is
        // carved out of the synced settings bundle; this pins that nothing outside it holds the credential,
        // which together are the whole "tokens are device-local, never synced" claim.
        //
        // Computed as booleans and never printed. The scan reads the ini back through QSettings so it sees
        // what was actually written rather than what we think we wrote.
        {
            QSettings check(srvIni, QSettings::IniFormat);
            bool foundOutsideJellyfin = false;
            bool foundInsideJellyfin  = false;
            for (const QString& k : check.allKeys())
            {
                const QString v = check.value(k).toString();
                const bool holds = v.contains(QStringLiteral("FIXTURE-TOKEN-A"))
                                || v.contains(QStringLiteral("FIXTURE-TOKEN-B"));
                if (!holds) continue;
                if (k.startsWith(QStringLiteral("jellyfin/"))) foundInsideJellyfin = true;
                else                                          foundOutsideJellyfin = true;
            }
            CHECK(foundInsideJellyfin);        // the store really did persist it (not a vacuous pass)
            CHECK(!foundOutsideJellyfin);      // and nothing else in the ini carries it
        }

        // A ROW WRITTEN BEFORE `enabled` EXISTED READS BACK AS ENABLED. Written here as raw JSON rather than
        // through add(), because that is the only way the absent-field case can occur, and the failure it
        // guards against is silent: every server in an upgrading install would hide its whole library with
        // nothing on screen to say why.
        {
            QSettings raw(srvIni, QSettings::IniFormat);
            QString key;
            for (const QString& k : raw.allKeys())
                if (k.endsWith(QStringLiteral("/servers"))) key = k;
            CHECK(!key.isEmpty());
            QJsonArray arr = QJsonDocument::fromJson(raw.value(key).toString().toUtf8()).array();
            for (int i = 0; i < arr.size(); ++i)
            {
                QJsonObject o = arr.at(i).toObject();
                o.remove(QStringLiteral("enabled"));
                arr.replace(i, o);
            }
            raw.setValue(key, QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)));
            raw.sync();
            JellyfinServerStore::setIniPathForTesting(srvIni);      // re-open, so the edit is what is read
            CHECK(JellyfinServerStore::list().size() == 2);
            CHECK(JellyfinServerStore::enabled().size() == 2);
        }

        // REMOVE forgets the sign-in and takes the token with it, in the same write.
        JellyfinServerStore::remove(QLatin1String(kSrvB));
        CHECK(JellyfinServerStore::list().size() == 1);
        CHECK(!JellyfinServerStore::get(QLatin1String(kSrvB), got));
        {
            QSettings check(srvIni, QSettings::IniFormat);
            check.sync();
            bool tokenBStillPresent = false;
            for (const QString& k : check.allKeys())
                if (check.value(k).toString().contains(QStringLiteral("FIXTURE-TOKEN-B")))
                    tokenBStillPresent = true;
            CHECK(!tokenBStillPresent);
        }
    }

    // =====================================================================================================
    // 9. THE MIGRATION, PURE HALF — the table, and rule 1 in both directions
    // =====================================================================================================
    {
        using namespace JellyfinMigrate;
        const QStringList rows = {
            legacyId,                                        // moves
            qualA,                                           // already qualified: absent (rule 4's basis)
            QStringLiteral("C:/Films/Blade Runner.mkv"),     // a local file: absent
            QStringLiteral("tt0083658"),                     // an addon item id: absent
            QString(),                                       // absent
        };
        const Table t = tableFor(rows, QString::fromLatin1(kSrvA));
        CHECK(t.map.size() == 1);
        CHECK(t.map.value(legacyId) == qualA);
        for (const QString& r : rows)
            if (r != legacyId) CHECK(!t.map.contains(r));
        // A destination is NEVER an empty string.
        for (auto it = t.map.cbegin(); it != t.map.cend(); ++it) CHECK(!it.value().isEmpty());

        // A malformed server id yields an EMPTY table rather than a table of half-formed destinations.
        CHECK(tableFor(rows, QStringLiteral("nope")).isEmpty());
        CHECK(tableFor(rows, QString()).isEmpty());

        // TWO SERVERS' TABLES SEND THE SAME LEGACY ROW TO DIFFERENT PLACES — which is why the migration
        // refuses to run at all when more than one server is configured (section 11).
        CHECK(tableFor({ legacyId }, QString::fromLatin1(kSrvB)).map.value(legacyId) == qualB);
        CHECK(qualA != qualB);
    }

    // =====================================================================================================
    // 10. THE MIGRATION, APPLIED — every store, and then the SAME RUN AGAIN
    // =====================================================================================================
    const QString ini = tmpDir() + QStringLiteral("/stores.ini");
    {
        QFile::remove(ini);
        JellyfinMigrate::setIniPathForTesting(ini);

        const QString prof = QStringLiteral("profileA");
        const QString dev  = QStringLiteral("device-1");
        {
            QSettings s(ini, QSettings::IniFormat);
            // A resume position, banked in the old shape.
            s.setValue(resumeGroup(legacyId) + QStringLiteral("/pos"), 1234);
            s.setValue(resumeGroup(legacyId) + QStringLiteral("/dur"), 5000);
            s.setValue(resumeGroup(legacyId) + QStringLiteral("/ts"), 1700000000);
            s.setValue(resumeGroup(legacyId) + QStringLiteral("/title"), QStringLiteral("Alien"));
            // A watched mark.
            s.setValue(marksKey(prof, legacyId),
                       QStringLiteral("{\"completion\":\"watched\",\"updatedAt\":1700000000}"));
            // Play stats, in BOTH the device-namespaced and the pre-namespacing shapes.
            s.setValue(playStatsKey(prof, dev, legacyId) + QStringLiteral("/total"), 600);
            s.setValue(playStatsKey(prof, dev, legacyId) + QStringLiteral("/sessions"), 2);
            s.setValue(playStatsKey(prof, dev, legacyId) + QStringLiteral("/last"), 1700000000);
            s.setValue(playStatsLegacyKey(prof, legacyId) + QStringLiteral("/total"), 60);
            s.setValue(playStatsLegacyKey(prof, legacyId) + QStringLiteral("/sessions"), 1);
            s.setValue(playStatsLegacyKey(prof, legacyId) + QStringLiteral("/last"), 1699000000);
            // RULE 3, MADE REAL: a record ALREADY under the qualified key. It can only get there by having
            // been accrued after a partial run, which is exactly the state a retry has to survive — so the
            // two are MERGED by arithmetic and neither is overwritten. Without a destination here, a mutant
            // that discarded one side would pass.
            s.setValue(playStatsKey(prof, dev, qualA) + QStringLiteral("/total"), 100);
            s.setValue(playStatsKey(prof, dev, qualA) + QStringLiteral("/sessions"), 1);
            s.setValue(playStatsKey(prof, dev, qualA) + QStringLiteral("/last"), 1690000000);

            // The three LITERAL stores — which are also the enumeration the table is built from.
            QJsonArray favs;
            { QJsonObject o; o.insert(QStringLiteral("itemId"), legacyId);
              o.insert(QStringLiteral("title"), QStringLiteral("Alien")); favs.append(o); }
            { QJsonObject o; o.insert(QStringLiteral("itemId"), QStringLiteral("tt0083658"));
              o.insert(QStringLiteral("title"), QStringLiteral("An addon row")); favs.append(o); }
            s.setValue(QStringLiteral("favorites/") + prof + QStringLiteral("/items"),
                       QString::fromUtf8(QJsonDocument(favs).toJson(QJsonDocument::Compact)));

            QJsonArray recents;
            { QJsonObject o; o.insert(QStringLiteral("key"), legacyId);
              o.insert(QStringLiteral("path"), legacyId);
              o.insert(QStringLiteral("title"), QStringLiteral("Alien")); recents.append(o); }
            { QJsonObject o; o.insert(QStringLiteral("path"), QStringLiteral("C:/Films/Solaris.mkv"));
              o.insert(QStringLiteral("title"), QStringLiteral("Solaris")); recents.append(o); }
            s.setValue(QStringLiteral("recent/") + prof + QStringLiteral("/items"),
                       QString::fromUtf8(QJsonDocument(recents).toJson(QJsonDocument::Compact)));

            QJsonArray playlists;
            { QJsonObject pl; pl.insert(QStringLiteral("id"), QStringLiteral("p1"));
              QJsonArray entries;
              { QJsonObject e; e.insert(QStringLiteral("itemId"), legacyId);
                e.insert(QStringLiteral("path"), legacyId); entries.append(e); }
              { QJsonObject e; e.insert(QStringLiteral("itemId"), QStringLiteral("tt0083658"));
                e.insert(QStringLiteral("path"), QStringLiteral("C:/Films/Solaris.mkv"));
                entries.append(e); }
              pl.insert(QStringLiteral("items"), entries); playlists.append(pl); }
            s.setValue(QStringLiteral("playlists/") + prof + QStringLiteral("/items"),
                       QString::fromUtf8(QJsonDocument(playlists).toJson(QJsonDocument::Compact)));
            s.sync();
        }

        // The enumeration finds the legacy reference — and finds it through the stores, not through a list
        // the caller happened to have.
        CHECK(JellyfinMigrate::storedIds().contains(legacyId));

        JellyfinMigrate::migrateSingleServer({ QString::fromLatin1(kSrvA) });

        auto readBack = [&](std::function<void(QSettings&)> body) {
            QSettings s(ini, QSettings::IniFormat); s.sync(); body(s);
        };

        readBack([&](QSettings& s) {
            // THE RESUME POSITION IS AT THE SAME PLACE, under the qualified key.
            CHECK(s.value(resumeGroup(qualA) + QStringLiteral("/pos")).toInt() == 1234);
            CHECK(s.value(resumeGroup(qualA) + QStringLiteral("/dur")).toInt() == 5000);
            CHECK(s.value(resumeGroup(qualA) + QStringLiteral("/title")).toString()
                  == QStringLiteral("Alien"));
            // …and the old key is gone, so nothing reads it twice.
            CHECK(!s.contains(resumeGroup(legacyId) + QStringLiteral("/pos")));

            CHECK(s.value(marksKey(prof, qualA)).toString().contains(QStringLiteral("watched")));
            CHECK(s.value(marksKey(prof, legacyId)).toString().isEmpty());

            // 600 moved + 100 already there. An accumulator MERGES BY ARITHMETIC: any other rule throws
            // away time that was genuinely spent. Sessions likewise; `last` takes the later of the two.
            CHECK(s.value(playStatsKey(prof, dev, qualA) + QStringLiteral("/total")).toLongLong() == 700);
            CHECK(s.value(playStatsKey(prof, dev, qualA) + QStringLiteral("/sessions")).toLongLong() == 3);
            CHECK(s.value(playStatsKey(prof, dev, qualA) + QStringLiteral("/last")).toLongLong()
                  == 1700000000);
            CHECK(!s.contains(playStatsKey(prof, dev, legacyId) + QStringLiteral("/total")));
            // The PRE-NAMESPACING shape moved too — it may well be folded away a moment later, and a record
            // this pass failed to see would be orphaned by that fold.
            CHECK(s.value(playStatsLegacyKey(prof, qualA) + QStringLiteral("/total")).toLongLong() == 60);
            CHECK(!s.contains(playStatsLegacyKey(prof, legacyId) + QStringLiteral("/total")));

            const QJsonArray favs = QJsonDocument::fromJson(
                s.value(QStringLiteral("favorites/") + prof + QStringLiteral("/items"))
                 .toString().toUtf8()).array();
            CHECK(favs.size() == 2);
            CHECK(favs.at(0).toObject().value(QStringLiteral("itemId")).toString() == qualA);
            // THE ADDON ROW BESIDE IT IS BYTE-FOR-BYTE UNTOUCHED. Rule 1's other direction.
            CHECK(favs.at(1).toObject().value(QStringLiteral("itemId")).toString()
                  == QStringLiteral("tt0083658"));

            const QJsonArray rec = QJsonDocument::fromJson(
                s.value(QStringLiteral("recent/") + prof + QStringLiteral("/items"))
                 .toString().toUtf8()).array();
            CHECK(rec.at(0).toObject().value(QStringLiteral("key")).toString() == qualA);
            CHECK(rec.at(0).toObject().value(QStringLiteral("path")).toString() == qualA);
            CHECK(rec.at(1).toObject().value(QStringLiteral("path")).toString()
                  == QStringLiteral("C:/Films/Solaris.mkv"));

            const QJsonArray pls = QJsonDocument::fromJson(
                s.value(QStringLiteral("playlists/") + prof + QStringLiteral("/items"))
                 .toString().toUtf8()).array();
            const QJsonArray entries = pls.at(0).toObject().value(QStringLiteral("items")).toArray();
            CHECK(entries.at(0).toObject().value(QStringLiteral("itemId")).toString() == qualA);
            CHECK(entries.at(0).toObject().value(QStringLiteral("path")).toString() == qualA);
            CHECK(entries.at(1).toObject().value(QStringLiteral("path")).toString()
                  == QStringLiteral("C:/Films/Solaris.mkv"));
        });

        // ---- RULE 4: RUNNING IT TWICE EQUALS RUNNING IT ONCE ---------------------------------------------
        // Not "it does not crash": the SAME position must still be there, under the SAME key, and nothing
        // may have been re-qualified into jf:<srv>:jf:<srv>:<item>. The second run must also find nothing to
        // enumerate, which is what makes idempotence structural rather than a stamp.
        CHECK(!JellyfinMigrate::storedIds().contains(legacyId));
        CHECK(JellyfinMigrate::tableFor(JellyfinMigrate::storedIds(),
                                        QString::fromLatin1(kSrvA)).isEmpty());

        JellyfinMigrate::migrateSingleServer({ QString::fromLatin1(kSrvA) });
        readBack([&](QSettings& s) {
            CHECK(s.value(resumeGroup(qualA) + QStringLiteral("/pos")).toInt() == 1234);
            // NOT 1400: a second run must not add the moved total to itself. This is the number an
            // idempotence bug changes first.
            CHECK(s.value(playStatsKey(prof, dev, qualA) + QStringLiteral("/total")).toLongLong() == 700);
            const QString doubled = Jellyfin::qualify(QString::fromLatin1(kSrvA), qualA);
            CHECK(!s.contains(resumeGroup(doubled) + QStringLiteral("/pos")));
            const QJsonArray favs = QJsonDocument::fromJson(
                s.value(QStringLiteral("favorites/") + prof + QStringLiteral("/items"))
                 .toString().toUtf8()).array();
            CHECK(favs.at(0).toObject().value(QStringLiteral("itemId")).toString() == qualA);
        });

        // A THIRD run, for the same reason a second one is not enough: an idempotence bug that alternates
        // would pass a two-run test.
        JellyfinMigrate::migrateSingleServer({ QString::fromLatin1(kSrvA) });
        readBack([&](QSettings& s) {
            CHECK(s.value(resumeGroup(qualA) + QStringLiteral("/pos")).toInt() == 1234);
        });
    }

    // =====================================================================================================
    // 11. WHICH SERVER? — the question that sometimes has no answer, and is then not guessed
    // =====================================================================================================
    {
        const QString amb = tmpDir() + QStringLiteral("/ambiguous.ini");
        QFile::remove(amb);
        JellyfinMigrate::setIniPathForTesting(amb);
        const QString prof = QStringLiteral("profileA");
        {
            QSettings s(amb, QSettings::IniFormat);
            s.setValue(resumeGroup(legacyId) + QStringLiteral("/pos"), 999);
            QJsonArray favs;
            QJsonObject o; o.insert(QStringLiteral("itemId"), legacyId); favs.append(o);
            s.setValue(QStringLiteral("favorites/") + prof + QStringLiteral("/items"),
                       QString::fromUtf8(QJsonDocument(favs).toJson(QJsonDocument::Compact)));
            s.sync();
        }

        // TWO servers configured: the row is ambiguous and attributing it to either would file one user's
        // position against the other's copy of the film. Nothing is written, and — the part that matters —
        // NOTHING IS DROPPED.
        JellyfinMigrate::migrateSingleServer({ QString::fromLatin1(kSrvA), QString::fromLatin1(kSrvB) });
        {
            QSettings s(amb, QSettings::IniFormat); s.sync();
            CHECK(s.value(resumeGroup(legacyId) + QStringLiteral("/pos")).toInt() == 999);
            CHECK(!s.contains(resumeGroup(qualA) + QStringLiteral("/pos")));
            CHECK(!s.contains(resumeGroup(qualB) + QStringLiteral("/pos")));
        }

        // NO server configured: same answer, same reason.
        JellyfinMigrate::migrateSingleServer({});
        {
            QSettings s(amb, QSettings::IniFormat); s.sync();
            CHECK(s.value(resumeGroup(legacyId) + QStringLiteral("/pos")).toInt() == 999);
        }

        // And once exactly ONE is configured, the row that was waiting moves. The pairing is the point: a
        // migration that refuses everything passes both "did not guess" checks by doing nothing at all.
        JellyfinMigrate::migrateSingleServer({ QString::fromLatin1(kSrvA) });
        {
            QSettings s(amb, QSettings::IniFormat); s.sync();
            CHECK(s.value(resumeGroup(qualA) + QStringLiteral("/pos")).toInt() == 999);
            CHECK(!s.contains(resumeGroup(legacyId) + QStringLiteral("/pos")));
        }
        QFile::remove(amb);
    }

    // =====================================================================================================
    // 12. AN INSTALL WITH NOTHING TO MIGRATE IS UNTOUCHED — a fact about control flow, not a claim
    // =====================================================================================================
    {
        const QString fresh = tmpDir() + QStringLiteral("/fresh.ini");
        QFile::remove(fresh);
        JellyfinMigrate::setIniPathForTesting(fresh);
        JellyfinMigrate::applyMigration(JellyfinMigrate::Table{});
        // applyMigration returns before it opens the store, so the ini is never even created.
        CHECK(!QFile::exists(fresh));
        QFile::remove(fresh);
    }

    // =====================================================================================================
    // 13. THE USER'S LIBRARIES (#83) - the reader, the collection mapping, and the union across servers
    // =====================================================================================================
    {
        bool ok = false;
        const QByteArray views = QByteArray(
            "{\"Items\":[{\"Id\":\"lib1\",\"Name\":\"Films\",\"CollectionType\":\"movies\"},"
            "{\"Id\":\"lib2\",\"Name\":\"Shows\",\"CollectionType\":\"tvshows\"},"
            "{\"Id\":\"lib3\",\"Name\":\"Records\",\"CollectionType\":\"music\"},"
            "{\"Id\":\"\",\"Name\":\"nameless\",\"CollectionType\":\"movies\"}]}");
        const QVector<Jellyfin::Library> libs = Jellyfin::readViews(views, &ok);
        CHECK(ok);
        CHECK(libs.size() == 3);                       // the id-less row is not a view
        CHECK(libs.at(0).collectionType == QStringLiteral("movies"));

        // A BODY THAT IS NOT A VIEWS ENVELOPE IS NOT "NO LIBRARIES". The same line readItems draws, and the
        // browse surface says different things for the two.
        bool bad = true;
        Jellyfin::readViews(QByteArray("<html>proxy error</html>"), &bad);
        CHECK(!bad);
        bool empty = false;
        CHECK(Jellyfin::readViews(QByteArray("{\"Items\":[]}"), &empty).isEmpty());
        CHECK(empty);                                  // ...and an empty list IS a real answer

        // The mapping onto this app's own categories, and the arm that matters most: an unrecognised
        // collection type is EMPTY, never the "video" catch-all - a boxsets/playlists view routed to Video
        // would be a folder whose every row this increment cannot open.
        CHECK(Jellyfin::categoryForCollection(QStringLiteral("movies")) == QStringLiteral("video"));
        CHECK(Jellyfin::categoryForCollection(QStringLiteral("tvshows")) == QStringLiteral("video"));
        CHECK(Jellyfin::categoryForCollection(QStringLiteral("music")) == QStringLiteral("audio"));
        CHECK(Jellyfin::categoryForCollection(QStringLiteral("boxsets")).isEmpty());
        CHECK(Jellyfin::categoryForCollection(QStringLiteral("livetv")).isEmpty());
        CHECK(Jellyfin::categoryForCollection(QString()).isEmpty());
        CHECK(Jellyfin::isVideoCollection(QStringLiteral("TVSHOWS")));   // case is the server's business
        CHECK(!Jellyfin::isVideoCollection(QStringLiteral("music")));

        // THE UNION, and the three properties it shares with unionOf: qualified ids, failure isolation as
        // the absence of a special case, and an unqualifiable row DROPPED rather than emitted bare.
        Jellyfin::LibraryReply a;
        a.serverId = QLatin1String(kSrvA); a.serverName = QStringLiteral("Attic");
        a.libraries = { { QStringLiteral("lib1"), QStringLiteral("Films"), QStringLiteral("movies") },
                        { QString(), QStringLiteral("broken"), QStringLiteral("movies") } };
        Jellyfin::LibraryReply b;
        b.serverId = QLatin1String(kSrvB); b.serverName = QStringLiteral("Basement");
        b.outcome = Jellyfin::Outcome::TimedOut;
        b.libraries = { { QStringLiteral("lib1"), QStringLiteral("Films"), QStringLiteral("movies") } };
        const QVector<Jellyfin::LibraryRef> merged = Jellyfin::unionOfLibraries({ a, b });
        CHECK(merged.size() == 1);                     // the timed-out server contributes NOTHING...
        CHECK(merged.at(0).serverName == QStringLiteral("Attic"));   // ...and blocks nothing
        CHECK(merged.at(0).ref == Jellyfin::qualify(QLatin1String(kSrvA), QStringLiteral("lib1")));
        // The id-less row was dropped, not emitted with a half-formed reference.
        for (const Jellyfin::LibraryRef& l : merged) CHECK(Jellyfin::isQualified(l.ref));
    }

    // =====================================================================================================
    // 14. SERIES STRUCTURE - the extra fields, and the parents qualified BY THE SAME MINTER
    // =====================================================================================================
    {
        bool ok = false;
        const QByteArray body = QByteArray(
            "{\"Items\":[{\"Id\":\"ep1\",\"Name\":\"The Cage\",\"Type\":\"Episode\","
            "\"SeriesName\":\"Trek\",\"SeriesId\":\"sh1\",\"SeasonId\":\"se1\","
            "\"IndexNumber\":4,\"ParentIndexNumber\":1,\"RunTimeTicks\":36000000000,"
            "\"UserData\":{\"Played\":false,\"PlaybackPositionTicks\":6000000000}}]}");
        const QVector<Jellyfin::RemoteItem> items = Jellyfin::readItems(body, &ok);
        CHECK(ok);
        CHECK(items.size() == 1);
        CHECK(items.at(0).indexNumber == 4);
        CHECK(items.at(0).parentIndexNumber == 1);
        CHECK(items.at(0).positionTicks == 6000000000LL);
        CHECK(items.at(0).seriesId == QStringLiteral("sh1"));
        CHECK(items.at(0).seasonId == QStringLiteral("se1"));

        Jellyfin::ServerReply r;
        r.serverId = QLatin1String(kSrvA); r.serverName = QStringLiteral("Attic"); r.items = items;
        const QVector<Jellyfin::UnionItem> u = Jellyfin::unionOf({ r });
        CHECK(u.size() == 1);
        CHECK(u.at(0).indexNumber == 4);
        CHECK(u.at(0).positionTicks == 6000000000LL);
        CHECK(u.at(0).runTimeTicks == 36000000000LL);
        // THE PARENTS ARE QUALIFIED, so a season row can be drilled and an episode re-opened into its own
        // show without anything downstream holding a bare, server-less id.
        CHECK(u.at(0).seriesRef == Jellyfin::qualify(QLatin1String(kSrvA), QStringLiteral("sh1")));
        CHECK(u.at(0).seasonRef == Jellyfin::qualify(QLatin1String(kSrvA), QStringLiteral("se1")));
        // ...and a row whose server named no parent carries an EMPTY reference, never a bare one.
        Jellyfin::ServerReply n;
        n.serverId = QLatin1String(kSrvA);
        n.items = { { QStringLiteral("m1"), QStringLiteral("Alien"), QStringLiteral("Movie") } };
        const QVector<Jellyfin::UnionItem> nu = Jellyfin::unionOf({ n });
        CHECK(nu.size() == 1);
        CHECK(nu.at(0).seriesRef.isEmpty());
        CHECK(nu.at(0).seasonRef.isEmpty());

        // The paths and queries, asserted so that a silent change to what is ASKED FOR shows up here.
        CHECK(Jellyfin::seasonsPath(QStringLiteral("sh1")) == QStringLiteral("/Shows/sh1/Seasons"));
        CHECK(Jellyfin::episodesPath(QStringLiteral("sh1")) == QStringLiteral("/Shows/sh1/Episodes"));
        CHECK(Jellyfin::viewsPath(QStringLiteral("u1")) == QStringLiteral("/Users/u1/Views"));
        CHECK(Jellyfin::itemPath(QStringLiteral("u1"), QStringLiteral("i2"))
              == QStringLiteral("/Users/u1/Items/i2"));
        CHECK(Jellyfin::resumeItemsPath(QStringLiteral("u1")) == QStringLiteral("/Users/u1/Items/Resume"));
        CHECK(Jellyfin::libraryItemsQuery(QStringLiteral("lib1"))
                  .contains(QStringLiteral("ParentId=lib1")));
        // A library lists TITLES, not episodes: a Series is a container fetched on open. Without this the
        // shelf for a two-hundred-show library is several thousand rows nobody asked for.
        CHECK(Jellyfin::libraryItemsQuery(QStringLiteral("lib1"))
                  .contains(QStringLiteral("IncludeItemTypes=Movie,Series")));
        // NO seasonId MEANS EVERY EPISODE, and an empty one must not become `seasonId=`, which the server
        // answers with nothing at all.
        CHECK(!Jellyfin::episodesQuery(QStringLiteral("u1"), QString())
                   .contains(QStringLiteral("seasonId")));
        CHECK(Jellyfin::episodesQuery(QStringLiteral("u1"), QStringLiteral("se1"))
                  .contains(QStringLiteral("seasonId=se1")));
        // Continue Watching is VIDEO ONLY (the music surface is #194's) and is capped, because it feeds one
        // section of the home list.
        CHECK(Jellyfin::resumeItemsQuery().contains(QStringLiteral("MediaTypes=Video")));
        CHECK(Jellyfin::resumeItemsQuery().contains(QStringLiteral("Limit=")));
    }

    // =====================================================================================================
    // 15. RESUME - the server is the authority, INCLUDING when it says zero
    // =====================================================================================================
    {
        const Jellyfin::UserState half =
            Jellyfin::readUserState(QByteArray("{\"Id\":\"i\",\"UserData\":"
                                               "{\"PlaybackPositionTicks\":6000000000,\"Played\":false}}"));
        CHECK(half.ok);
        CHECK(half.positionTicks == 6000000000LL);
        CHECK(!half.played);
        CHECK(qAbs(Jellyfin::secondsFromTicks(half.positionTicks) - 600.0) < 0.001);

        // "THE SERVER SAID NOTHING" AND "THE SERVER SAID ZERO" ARE DIFFERENT FACTS, and this is the whole
        // reason UserState carries `ok` at all.
        const Jellyfin::UserState silent = Jellyfin::readUserState(QByteArray("{\"Id\":\"i\"}"));
        CHECK(!silent.ok);
        const Jellyfin::UserState finished =
            Jellyfin::readUserState(QByteArray("{\"UserData\":{\"PlaybackPositionTicks\":0,"
                                               "\"Played\":true}}"));
        CHECK(finished.ok);
        CHECK(finished.played);

        // The rule. A server that answered wins - and a server that answered ZERO wins too, which is the
        // case this exists for: a film finished on a phone reports zero, and a local mark that beat it
        // would restart every re-watch two minutes from the end.
        CHECK(qAbs(Jellyfin::resumeSeconds(half, 12.0) - 600.0) < 0.001);
        CHECK(qAbs(Jellyfin::resumeSeconds(finished, 3000.0) - 0.0) < 0.001);
        // ...and a server that did NOT answer leaves the local mark alone, rather than punishing a network
        // hiccup by restarting a half-watched film.
        CHECK(qAbs(Jellyfin::resumeSeconds(silent, 3000.0) - 3000.0) < 0.001);
        CHECK(qAbs(Jellyfin::resumeSeconds(silent, -5.0) - 0.0) < 0.001);   // ...and never a negative seek
    }

    // =====================================================================================================
    // 16. PLAYBACKINFO - THE SERVER DECIDES, and the url it produces
    // =====================================================================================================
    {
        const QString root = QStringLiteral("https://jf.example.com");
        const QString tok  = QStringLiteral("FIXTURE-TOKEN-A");

        // Direct play. The FIRST media source decides; the play-session id is on the ENVELOPE.
        const Jellyfin::PlaybackChoice direct = Jellyfin::readPlaybackInfo(QByteArray(
            "{\"PlaySessionId\":\"ps1\",\"MediaSources\":[{\"Id\":\"ms1\",\"Container\":\"mkv\","
            "\"SupportsDirectPlay\":true,\"SupportsDirectStream\":true,\"SupportsTranscoding\":true}]}"));
        CHECK(direct.ok);
        CHECK(direct.mode == Jellyfin::PlaybackChoice::Mode::DirectPlay);
        CHECK(direct.playSessionId == QStringLiteral("ps1"));
        CHECK(direct.mediaSourceId == QStringLiteral("ms1"));

        // The server refuses both direct routes: we take ITS transcode url, not one of our own.
        const Jellyfin::PlaybackChoice trans = Jellyfin::readPlaybackInfo(QByteArray(
            "{\"PlaySessionId\":\"ps2\",\"MediaSources\":[{\"Id\":\"ms2\","
            "\"SupportsDirectPlay\":false,\"SupportsDirectStream\":false,\"SupportsTranscoding\":true,"
            "\"TranscodingUrl\":\"/videos/ms2/master.m3u8?PlaySessionId=ps2&VideoCodec=h264\"}]}"));
        CHECK(trans.ok);
        CHECK(trans.mode == Jellyfin::PlaybackChoice::Mode::Transcode);

        // Neither, and nothing to fall back on: an honest refusal, not an empty player.
        const Jellyfin::PlaybackChoice none = Jellyfin::readPlaybackInfo(QByteArray(
            "{\"MediaSources\":[{\"Id\":\"ms3\",\"SupportsDirectPlay\":false,"
            "\"SupportsDirectStream\":false}]}"));
        CHECK(none.ok);
        CHECK(none.mode == Jellyfin::PlaybackChoice::Mode::Unavailable);
        CHECK(Jellyfin::playbackUrl(root, QStringLiteral("i1"), tok, none).isEmpty());
        // An envelope with no sources at all is not a playable answer either.
        CHECK(!Jellyfin::readPlaybackInfo(QByteArray("{\"MediaSources\":[]}")).ok);
        CHECK(!Jellyfin::readPlaybackInfo(QByteArray("<html>nope</html>")).ok);

        // THE DIRECT URL. Named source and play session, so a two-version film plays the version the user
        // picked and the server's own session list shows ONE playback.
        const QString du = Jellyfin::playbackUrl(root, QStringLiteral("i1"), tok, direct);
        CHECK(du.startsWith(root + QStringLiteral("/Videos/i1/stream")));
        CHECK(du.contains(QStringLiteral("mediaSourceId=ms1")));
        CHECK(du.contains(QStringLiteral("playSessionId=ps1")));
        // Asserted as a BOOLEAN computed here, so the token never reaches a printed expression.
        const bool directCarriesToken = du.contains(QStringLiteral("api_key=FIXTURE-TOKEN-A"));
        CHECK(directCarriesToken);

        // THE TRANSCODE URL IS THE SERVER'S OWN, and the token is appended only when it is not already
        // there: Jellyfin reads the FIRST api_key, so a second would leave us playing a url that does not
        // match the session the server thinks it opened.
        const QString tu = Jellyfin::playbackUrl(root, QStringLiteral("i1"), tok, trans);
        CHECK(tu.startsWith(root + QStringLiteral("/videos/ms2/master.m3u8")));
        CHECK(tu.count(QStringLiteral("api_key=")) == 1);
        Jellyfin::PlaybackChoice already = trans;
        already.transcodingUrl = QStringLiteral("/videos/ms2/master.m3u8?api_key=SERVERS-OWN&x=1");
        const QString au = Jellyfin::playbackUrl(root, QStringLiteral("i1"), tok, already);
        CHECK(au.count(QStringLiteral("api_key=")) == 1);
        const bool ourTokenNotAppended = !au.contains(QStringLiteral("api_key=FIXTURE-TOKEN-A"));
        CHECK(ourTokenNotAppended);

        // The request body: all three routes offered (the server chooses), and a stream index that the
        // user has NOT chosen is ABSENT rather than -1 - sending -1 pins the choice to "no track at all",
        // which for subtitles suppresses the server's own default.
        const QByteArray plain = Jellyfin::playbackInfoBody(QStringLiteral("u1"), 0, -1, -1);
        CHECK(plain.contains("\"EnableDirectPlay\":true"));
        CHECK(plain.contains("\"EnableTranscoding\":true"));
        CHECK(!plain.contains("AudioStreamIndex"));
        CHECK(!plain.contains("SubtitleStreamIndex"));
        const QByteArray picked = Jellyfin::playbackInfoBody(QStringLiteral("u1"), 100, 2, 3);
        CHECK(picked.contains("\"AudioStreamIndex\":2"));
        CHECK(picked.contains("\"SubtitleStreamIndex\":3"));
        CHECK(picked.contains("\"StartTimeTicks\":100"));
    }

    // =====================================================================================================
    // 17. PROGRESS - the endpoints, the body, and the throttle
    // =====================================================================================================
    {
        CHECK(Jellyfin::progressPath(Jellyfin::ProgressEvent::Start)
              == QStringLiteral("/Sessions/Playing"));
        CHECK(Jellyfin::progressPath(Jellyfin::ProgressEvent::Stop)
              == QStringLiteral("/Sessions/Playing/Stopped"));
        // A pause and an unpause are ORDINARY progress reports carrying IsPaused, which is how Jellyfin's
        // own clients spell them - not endpoints of their own.
        CHECK(Jellyfin::progressPath(Jellyfin::ProgressEvent::Progress)
              == QStringLiteral("/Sessions/Playing/Progress"));
        CHECK(Jellyfin::progressPath(Jellyfin::ProgressEvent::Pause)
              == QStringLiteral("/Sessions/Playing/Progress"));
        CHECK(Jellyfin::progressPath(Jellyfin::ProgressEvent::Unpause)
              == QStringLiteral("/Sessions/Playing/Progress"));

        const QByteArray b = Jellyfin::progressBody(QStringLiteral("i1"), QStringLiteral("ps1"),
                                                    QStringLiteral("ms1"), 600.0,
                                                    Jellyfin::ProgressEvent::Progress);
        // BOTH IDS RIDE EVERY REPORT: without the play session the server files it against no playback,
        // without the media source a multi-version item records against whichever version it guesses.
        CHECK(b.contains("\"PlaySessionId\":\"ps1\""));
        CHECK(b.contains("\"MediaSourceId\":\"ms1\""));
        CHECK(b.contains("\"PositionTicks\":6000000000"));
        CHECK(b.contains("\"IsPaused\":false"));
        const QByteArray p = Jellyfin::progressBody(QStringLiteral("i1"), QStringLiteral("ps1"),
                                                    QStringLiteral("ms1"), 600.0,
                                                    Jellyfin::ProgressEvent::Pause);
        CHECK(p.contains("\"IsPaused\":true"));

        // The throttle. Nothing reported yet goes out at once - the first tick is what tells the server the
        // playback is real.
        CHECK(Jellyfin::shouldReportProgress(-1.0, 0.0));
        CHECK(!Jellyfin::shouldReportProgress(100.0, 105.0));
        CHECK(Jellyfin::shouldReportProgress(100.0, 110.0));
        // A BACKWARD SEEK REPORTS IMMEDIATELY: the rule is on the ABSOLUTE difference, so scrubbing back
        // and leaving cannot leave the server holding a position ahead of where the user stopped.
        CHECK(Jellyfin::shouldReportProgress(100.0, 60.0));
        CHECK(!Jellyfin::shouldReportProgress(100.0, 95.0));
    }

    // =====================================================================================================
    // 18. MEDIA SEGMENTS - one more provider tier, in seconds
    // =====================================================================================================
    {
        const QVector<Jellyfin::RemoteSegment> segs = Jellyfin::readMediaSegments(QByteArray(
            "{\"Items\":[{\"Type\":\"Intro\",\"StartTicks\":100000000,\"EndTicks\":900000000},"
            "{\"Type\":\"Outro\",\"StartTicks\":36000000000,\"EndTicks\":37200000000},"
            "{\"Type\":\"Intro\",\"StartTicks\":500000000,\"EndTicks\":500000000},"
            "{\"Type\":\"Intro\",\"StartTicks\":900000000,\"EndTicks\":100000000},"
            "{\"StartTicks\":100,\"EndTicks\":200000000}]}"));
        // Two survive: the zero-length one, the inverted one and the type-less one are dropped, because a
        // range that goes nowhere arms a skip that jumps nowhere - which reads as the chip being broken.
        CHECK(segs.size() == 2);
        CHECK(qAbs(segs.at(0).start - 10.0) < 0.001);
        CHECK(qAbs(segs.at(0).end - 90.0) < 0.001);
        CHECK(segs.at(1).type == QStringLiteral("Outro"));
        CHECK(Jellyfin::readMediaSegments(QByteArray("<html>404</html>")).isEmpty());
        CHECK(Jellyfin::mediaSegmentsPath(QStringLiteral("i1")) == QStringLiteral("/MediaSegments/i1"));
        CHECK(Jellyfin::mediaSegmentsQuery().contains(QStringLiteral("includeSegmentTypes=Intro")));
        CHECK(Jellyfin::mediaSegmentsQuery().contains(QStringLiteral("includeSegmentTypes=Outro")));
    }

    // =====================================================================================================
    // 19. THE CREDENTIAL: WHAT A ROW RECORDS, AND A BYTE SCAN OF WHAT REACHED THE DISK
    // =====================================================================================================
    // The claim under test is not "we intend not to store the token". It is: drive the REAL store the app
    // writes recents through, then read every byte of the file it produced and find no token in it. The
    // vacuity control comes first - a scan that cannot see a token would pass this even if the whole rule
    // were deleted.
    {
        const QString tok = QStringLiteral("FIXTURE-TOKEN-A");
        const QString root = QStringLiteral("https://jf.example.com");
        Jellyfin::PlaybackChoice direct;
        direct.ok = true; direct.mode = Jellyfin::PlaybackChoice::Mode::DirectPlay;
        direct.mediaSourceId = QStringLiteral("ms1"); direct.playSessionId = QStringLiteral("ps1");
        const QString url = Jellyfin::playbackUrl(root, QLatin1String(kItem), tok, direct);
        CHECK(!url.isEmpty());

        // THE RULE. A qualified id is what a row records; anything else is returned byte for byte, so no
        // other route in the app changes at all.
        CHECK(Jellyfin::recordedPath(qualA, url) == qualA);
        CHECK(Jellyfin::recordedPath(QString(), url) == url);
        CHECK(Jellyfin::recordedPath(QStringLiteral("C:/Films/Alien.mkv"), url) == url);
        const bool recordedIsTokenFree = !Jellyfin::recordedPath(qualA, url).contains(tok);
        CHECK(recordedIsTokenFree);

        // --- The vacuity control: a scanner that CAN see a token in a file. ---
        const QString control = tmpDir() + QStringLiteral("/scan-control.ini");
        QFile::remove(control);
        {
            QSettings c(control, QSettings::IniFormat);
            c.setValue(QStringLiteral("control/url"), url);
            c.sync();
        }
        bool controlSaw = false;
        {
            QFile f(control);
            if (f.open(QIODevice::ReadOnly)) controlSaw = f.readAll().contains(tok.toUtf8());
        }
        CHECK(controlSaw);       // the scan below is not vacuous
        QFile::remove(control);  // ...and the fixture credential does not outlive the check

        // --- The real thing: RecentStore, the store the app's play sites write through. ---
        // EB_ISOLATED_DATA_DIR puts this probe's ini in its own directory, so this writes nowhere near a
        // user's settings.
        RecentStore::add({ Jellyfin::recordedPath(qualA, url), QStringLiteral("Alien"),
                           QStringLiteral("video"), QString(), qualA });
        bool foundRow = false;
        for (const RecentItem& r : RecentStore::list())
            if (r.key == qualA) { foundRow = true; CHECK(r.path == qualA); }
        CHECK(foundRow);         // the row really was written (not a vacuous pass)

        const QString dataIni = AppPaths::dataDir() + QStringLiteral("/")
                              + QLatin1String(AppBrand::kIniFile);
        QFile f(dataIni);
        bool tokenOnDisk = true;
        if (f.open(QIODevice::ReadOnly)) tokenOnDisk = f.readAll().contains(tok.toUtf8());
        // EVERY BYTE OF THE FILE, not a key-by-key walk: a token smuggled into a comment, a section name or
        // a value this probe did not think to look at is still a token on disk.
        CHECK(!tokenOnDisk);
    }

    // =====================================================================================================
    // 20. QUICK CONNECT (issue #83): offer it or not, the code, the poll, and the stop
    // =====================================================================================================
    // The pure decisions first, over a table, then the REAL session against FakeJellyfin on 127.0.0.1.
    // The fixture token is compared and never printed: every credential check is a boolean computed first.
    {
        using namespace JellyfinQuickConnect;

        // ---- 20a. Offer Quick Connect, or the password? Only an exact 200 `true` offers it. ----
        CHECK(routeFor(true, 200, "true") == Route::QuickConnect);
        CHECK(routeFor(true, 200, " true\n") == Route::QuickConnect);
        CHECK(routeFor(true, 200, "false") == Route::Password);
        CHECK(routeFor(true, 200, "\"true\"") == Route::Password);
        CHECK(routeFor(true, 200, "<html>true</html>") == Route::Password);
        CHECK(routeFor(false, 404, "") == Route::Password);        // 10.7: no such route
        CHECK(routeFor(false, 0, "") == Route::Password);          // unreachable / timed out
        CHECK(routeFor(true, 503, "true") == Route::Password);

        // ---- 20b. What a poll answer means. ----
        CHECK(stateFor(true, 200, "{\"Authenticated\":true,\"Secret\":\"s\"}") == State::Authorized);
        CHECK(stateFor(true, 200, "{\"Authenticated\":false,\"Secret\":\"s\"}") == State::Waiting);
        CHECK(stateFor(false, 404, "") == State::Expired);         // "Unknown secret"
        CHECK(stateFor(false, 0, "") == State::Waiting);           // a missed poll is not a verdict
        CHECK(stateFor(false, 502, "") == State::Waiting);
        CHECK(stateFor(false, 401, "") == State::Error);           // switched off while we waited
        CHECK(stateFor(true, 200, "{\"Secret\":\"s\"}") == State::Error);          // not a QuickConnectResult
        CHECK(stateFor(true, 200, "{\"Authenticated\":\"true\"}") == State::Error); // a string is not a bool
        CHECK(stateFor(true, 200, "<html>captive portal</html>") == State::Error);

        // ---- 20c. Initiate's reader, the verb fallback, the lifetime, the spellings. ----
        const Code c = readInitiate("{\"Secret\":\"abc\",\"Code\":\"123456\",\"Authenticated\":false}");
        CHECK(c.ok && c.secret == QStringLiteral("abc") && c.code == QStringLiteral("123456"));
        CHECK(!readInitiate("{\"Code\":\"123456\"}").ok);
        CHECK(!readInitiate("{\"Secret\":\"abc\"}").ok);
        CHECK(initiateRetryAsGet(405) && initiateRetryAsGet(404));
        CHECK(!initiateRetryAsGet(401) && !initiateRetryAsGet(200) && !initiateRetryAsGet(500));
        CHECK(!timedOut(kLifetimeMs - 1) && timedOut(kLifetimeMs));
        CHECK(kPollIntervalMs == 2000 && kLifetimeMs == 300000);
        CHECK(connectQuery(QStringLiteral("a&b")) == QStringLiteral("secret=a%26b"));
        CHECK(QJsonDocument::fromJson(authenticateBody(QStringLiteral("abc"))).object()
                  .value(QStringLiteral("Secret")).toString() == QStringLiteral("abc"));
        CHECK(authenticatePath() == QStringLiteral("/Users/AuthenticateWithQuickConnect"));

        // ---- The fixtures the socket half runs against. ----
        const QString qcToken = QStringLiteral("qc0fixture") + QString::fromLatin1(kItem);
        const QByteArray authBody = QByteArray("{\"User\":{\"Id\":\"u-qc-1\",\"Name\":\"parker\"},"
                                               "\"SessionInfo\":{},\"AccessToken\":\"")
                                  + qcToken.toUtf8() + "\",\"ServerId\":\"" + kSrvA + "\"}";
        QNetworkAccessManager nam;
        const JellyfinQuickConnect::Decorate decorate = [](QNetworkRequest& req) {
            req.setRawHeader("Authorization", Jellyfin::authHeader(QStringLiteral("EverythingBox"),
                QStringLiteral("probe"), QStringLiteral("probe-device"), QStringLiteral("0"), QString()).toUtf8());
        };
        struct Outcome
        {
            QStringList codes; int authorized = 0; int expired = 0; int failed = 0;
            Jellyfin::AuthResult result;
        };
        auto wire = [](JellyfinQuickConnectSession* s, Outcome* o) {
            QObject::connect(s, &JellyfinQuickConnectSession::codeReady, [o](const QString& code) { o->codes << code; });
            QObject::connect(s, &JellyfinQuickConnectSession::authorized,
                             [o](const Jellyfin::AuthResult& r) { ++o->authorized; o->result = r; });
            QObject::connect(s, &JellyfinQuickConnectSession::expired, [o] { ++o->expired; });
            QObject::connect(s, &JellyfinQuickConnectSession::failed, [o](const QString&) { ++o->failed; });
        };
        auto routeOf = [&](FakeJellyfin& srv, bool* called) {
            Route r = Route::QuickConnect;
            QObject ctx;
            *called = false;
            JellyfinQuickConnect::fetchRoute(&nam, srv.root(), decorate, 3000, &ctx,
                                             [&](Route got) { r = got; *called = true; });
            waitFor([&] { return *called; });
            return r;
        };
        auto connects = [](const FakeJellyfin& srv) {
            return srv.countOf(QStringLiteral("GET"), QStringLiteral("/QuickConnect/Connect"));
        };
        auto settled = [](const Outcome& o) { return o.authorized + o.expired + o.failed > 0; };

        // ---- 20d. QC ENABLED: the route, the code, two pending polls, authorized, stored like a password. ----
        {
            FakeJellyfin srv;
            CHECK(srv.listen(QHostAddress::LocalHost, 0));
            srv.authBody = authBody;
            bool called = false;
            CHECK(routeOf(srv, &called) == Route::QuickConnect);
            CHECK(called);
            CHECK(srv.countOf(QStringLiteral("GET"), QStringLiteral("/QuickConnect/Enabled")) == 1);

            Outcome o;
            JellyfinQuickConnectSession s(&nam, srv.root(), decorate);
            s.setPollIntervalMs(40);
            wire(&s, &o);
            s.start();
            CHECK(waitFor([&] { return settled(o); }));
            CHECK(o.codes == QStringList{ QString::fromLatin1(FakeJellyfin::kCode) });   // the code is shown
            CHECK(o.authorized == 1 && o.expired == 0 && o.failed == 0);
            // Two pending polls, then the authorizing one: exactly three Connect calls, and no fourth.
            CHECK(connects(srv) == 3);
            CHECK(s.pollCount() == 3);
            CHECK(!s.isActive());
            spinFor(250);
            CHECK(connects(srv) == 3);
            // Initiate was a POST, carrying the client/device header (the server keys the request on it).
            const FakeJellyfin::Seen* init = srv.lastOf(QStringLiteral("POST"), QStringLiteral("/QuickConnect/Initiate"));
            CHECK(init && init->hasAuthHeader);
            CHECK(srv.countOf(QStringLiteral("POST"), QStringLiteral("/Users/AuthenticateWithQuickConnect")) == 1);

            // THE SAME STORAGE PATH AS A PASSWORD SIGN-IN. The password route reads AuthenticateByName's
            // answer with readAuthResult and stores fromSignIn's record; the Quick Connect result must
            // produce the identical record, field for field, and the stored token must be the fixture's.
            Jellyfin::PublicInfo info;
            info.serverId = QString::fromLatin1(kSrvA); info.serverName = QStringLiteral("Attic"); info.ok = true;
            const QString url = srv.root();
            const JellyfinServer viaQc = JellyfinServerStore::fromSignIn(info, o.result, url, true);
            const JellyfinServer viaPw = JellyfinServerStore::fromSignIn(info, Jellyfin::readAuthResult(authBody),
                                                                         url, true);
            const bool sameRecord = viaQc.id == viaPw.id && viaQc.name == viaPw.name && viaQc.url == viaPw.url
                && viaQc.userId == viaPw.userId && viaQc.userName == viaPw.userName
                && viaQc.token == viaPw.token && viaQc.allowPlainHttp == viaPw.allowPlainHttp
                && viaQc.enabled == viaPw.enabled;
            CHECK(sameRecord);
            const QString qcIni = tmpDir() + QStringLiteral("/qc-servers.ini");
            QFile::remove(qcIni);
            JellyfinServerStore::setIniPathForTesting(qcIni);
            CHECK(JellyfinServerStore::add(viaQc));
            JellyfinServer back;
            CHECK(JellyfinServerStore::get(QString::fromLatin1(kSrvA), back));
            const bool tokenStored = back.token == qcToken;
            CHECK(tokenStored);
            CHECK(back.userId == QStringLiteral("u-qc-1") && back.userName == QStringLiteral("parker"));
            CHECK(back.name == QStringLiteral("Attic") && back.allowPlainHttp);
            JellyfinServerStore::setIniPathForTesting(srvIni);
            QFile::remove(qcIni);
        }

        // ---- 20e. QC DISABLED -> the password flow. Nothing past Enabled is asked. ----
        {
            FakeJellyfin srv;
            CHECK(srv.listen(QHostAddress::LocalHost, 0));
            srv.enabledBody = "false";
            bool called = false;
            CHECK(routeOf(srv, &called) == Route::Password);
            CHECK(called);
            CHECK(srv.countOf(QStringLiteral("POST"), QStringLiteral("/QuickConnect/Initiate")) == 0);
        }

        // ---- 20f. Enabled ERRORS -> the password flow: a 404 from an old server, and nobody listening. ----
        {
            FakeJellyfin srv;
            CHECK(srv.listen(QHostAddress::LocalHost, 0));
            srv.enabledStatus = 404;
            srv.enabledBody   = "";
            bool called = false;
            CHECK(routeOf(srv, &called) == Route::Password);
            CHECK(called);

            QString deadRoot;
            {
                FakeJellyfin gone;
                CHECK(gone.listen(QHostAddress::LocalHost, 0));
                deadRoot = gone.root();
                gone.close();
            }
            Route r = Route::QuickConnect;
            bool deadCalled = false;
            QObject ctx;
            JellyfinQuickConnect::fetchRoute(&nam, deadRoot, decorate, 3000, &ctx,
                                             [&](Route got) { r = got; deadCalled = true; });
            CHECK(waitFor([&] { return deadCalled; }));
            CHECK(r == Route::Password);
        }

        // ---- 20g. EXPIRED or UNKNOWN secret -> expired, once, and the polling stops. ----
        {
            FakeJellyfin srv;
            CHECK(srv.listen(QHostAddress::LocalHost, 0));
            srv.connectStatus = 404;
            Outcome o;
            JellyfinQuickConnectSession s(&nam, srv.root(), decorate);
            s.setPollIntervalMs(40);
            wire(&s, &o);
            s.start();
            CHECK(waitFor([&] { return settled(o); }));
            CHECK(o.expired == 1 && o.authorized == 0 && o.failed == 0);
            CHECK(connects(srv) == 1);
            spinFor(250);
            CHECK(connects(srv) == 1);
            CHECK(srv.countOf(QStringLiteral("POST"), QStringLiteral("/Users/AuthenticateWithQuickConnect")) == 0);
        }

        // ---- 20h. TIMEOUT: never approved -> expired after the lifetime, and nothing after it. ----
        {
            FakeJellyfin srv;
            CHECK(srv.listen(QHostAddress::LocalHost, 0));
            srv.pendingPolls = 1000000;
            Outcome o;
            JellyfinQuickConnectSession s(&nam, srv.root(), decorate);
            s.setPollIntervalMs(30);
            s.setLifetimeMs(400);
            wire(&s, &o);
            s.start();
            CHECK(waitFor([&] { return settled(o); }, 5000));
            CHECK(o.expired == 1 && o.authorized == 0 && o.failed == 0);
            CHECK(s.pollCount() >= 2);                     // it really did poll while it waited
            const int polls = connects(srv);
            spinFor(250);
            CHECK(connects(srv) == polls);
        }

        // ---- 20i. CANCEL stops polling: the server sees no further Connect calls. And so does closing
        //           the panel that owns the session, which is how the app cancels on Back. ----
        {
            FakeJellyfin srv;
            CHECK(srv.listen(QHostAddress::LocalHost, 0));
            srv.pendingPolls = 1000000;
            Outcome o;
            JellyfinQuickConnectSession s(&nam, srv.root(), decorate);
            s.setPollIntervalMs(30);
            wire(&s, &o);
            s.start();
            CHECK(waitFor([&] { return connects(srv) >= 2; }));
            s.cancel();
            // A request already on the wire when cancel() ran may still reach the server; nothing after it.
            spinFor(150);
            const int polls = connects(srv);
            spinFor(400);
            CHECK(connects(srv) == polls);
            CHECK(!s.isActive());
            CHECK(o.authorized == 0 && o.expired == 0 && o.failed == 0);   // a cancel says nothing

            FakeJellyfin srv2;
            CHECK(srv2.listen(QHostAddress::LocalHost, 0));
            srv2.pendingPolls = 1000000;
            auto* panel = new QObject;
            auto* owned = new JellyfinQuickConnectSession(&nam, srv2.root(), decorate, panel);
            owned->setPollIntervalMs(30);
            owned->start();
            CHECK(waitFor([&] { return connects(srv2) >= 2; }));
            delete panel;                                  // the panel closes
            spinFor(150);
            const int after = connects(srv2);
            spinFor(400);
            CHECK(connects(srv2) == after);
        }

        // ---- 20j. A 10.8 server: POST Initiate answers 405, the GET retry gets the code. ----
        {
            FakeJellyfin srv;
            CHECK(srv.listen(QHostAddress::LocalHost, 0));
            srv.initiateGetOnly = true;
            srv.authBody = authBody;
            srv.pendingPolls = 0;
            Outcome o;
            JellyfinQuickConnectSession s(&nam, srv.root(), decorate);
            s.setPollIntervalMs(30);
            wire(&s, &o);
            s.start();
            CHECK(waitFor([&] { return settled(o); }));
            CHECK(o.codes.size() == 1 && o.authorized == 1);
            CHECK(srv.countOf(QStringLiteral("POST"), QStringLiteral("/QuickConnect/Initiate")) == 1);
            CHECK(srv.countOf(QStringLiteral("GET"), QStringLiteral("/QuickConnect/Initiate")) == 1);
        }

        // ---- 20k. Switched off after Enabled said yes -> failed, never a code that cannot work. ----
        {
            FakeJellyfin srv;
            CHECK(srv.listen(QHostAddress::LocalHost, 0));
            srv.connectStatus = 401;
            Outcome o;
            JellyfinQuickConnectSession s(&nam, srv.root(), decorate);
            s.setPollIntervalMs(30);
            wire(&s, &o);
            s.start();
            CHECK(waitFor([&] { return settled(o); }));
            CHECK(o.failed == 1 && o.authorized == 0 && o.expired == 0);

            FakeJellyfin off;
            CHECK(off.listen(QHostAddress::LocalHost, 0));
            off.initiateStatus = 401;
            Outcome o2;
            JellyfinQuickConnectSession s2(&nam, off.root(), decorate);
            wire(&s2, &o2);
            s2.start();
            CHECK(waitFor([&] { return settled(o2); }));
            CHECK(o2.failed == 1 && o2.codes.isEmpty());
            CHECK(connects(off) == 0);
        }
    }

    // =====================================================================================================
    // 21. "SHOW ONLY <SERVER>" AND THE CONTINUE WATCHING SHAPE — the pure rules (issue #160, increment 2)
    // =====================================================================================================
    // Both are policy, and both are asserted here with no store, no socket and no widget. The store section
    // below proves what is REMEMBERED; section 23 proves what the fan-out then does with it.
    {
        auto choice = [](const char* id, const char* name) {
            Jellyfin::ServerChoice c;
            c.id = QString::fromLatin1(id); c.name = QString::fromLatin1(name);
            return c;
        };
        const QVector<Jellyfin::ServerChoice> two  { choice(kSrvA, "Attic"), choice(kSrvB, "Loft") };
        const QVector<Jellyfin::ServerChoice> one  { choice(kSrvA, "Attic") };
        const QStringList bothContributed { QString::fromLatin1(kSrvA), QString::fromLatin1(kSrvB) };
        const QStringList onlyA           { QString::fromLatin1(kSrvA) };

        // ---- 21a. WHEN IT IS OFFERED AT ALL. ----
        // One server: never. This is the "with one, nothing new appears" half of the decision, and it is
        // what keeps a single-server user's browse root exactly as it was.
        CHECK(Jellyfin::serverFilterChoices(one, onlyA, QString()).isEmpty());
        CHECK(Jellyfin::serverFilterChoices({}, {}, QString()).isEmpty());
        // Two enabled but only ONE contributed — the friend's box is off at the wall. Still nothing: what
        // is on the screen is already one server's worth, and a filter over it changes nothing.
        CHECK(Jellyfin::serverFilterChoices(two, onlyA, QString()).isEmpty());
        CHECK(Jellyfin::serverFilterChoices(two, {}, QString()).isEmpty());
        // A contributor list that names the SAME server twice is one server.
        CHECK(Jellyfin::serverFilterChoices(two, QStringList{ QString::fromLatin1(kSrvA),
                                                              QString::fromLatin1(kSrvA) },
                                            QString()).isEmpty());
        // Two contributed: offered.
        const QVector<Jellyfin::ServerChoice> all = Jellyfin::serverFilterChoices(two, bothContributed,
                                                                                  QString());
        CHECK(all.size() == 3);
        if (all.size() == 3)
        {
            // ALL SERVERS FIRST, and it is the one marked current while nothing is filtered.
            CHECK(all[0].id.isEmpty() && all[0].name.isEmpty() && all[0].current);
            // ...then one per enabled server, in the STORE's order, neither of them current.
            CHECK(all[1].id == QLatin1String(kSrvA) && all[1].name == QStringLiteral("Attic") && !all[1].current);
            CHECK(all[2].id == QLatin1String(kSrvB) && all[2].name == QStringLiteral("Loft") && !all[2].current);
        }

        // ---- 21b. WITH A FILTER ON, the list is offered even though only one server contributed. ----
        // The one-way-door case: built from contributors alone, a filtered root would offer no way back.
        const QVector<Jellyfin::ServerChoice> filtered =
            Jellyfin::serverFilterChoices(two, QStringList{ QString::fromLatin1(kSrvB) },
                                          QString::fromLatin1(kSrvB));
        CHECK(filtered.size() == 3);
        if (filtered.size() == 3)
        {
            CHECK(!filtered[0].current);                       // "all servers" is a destination now
            CHECK(filtered[0].id.isEmpty());
            CHECK(filtered[1].id == QLatin1String(kSrvA) && !filtered[1].current);
            CHECK(filtered[2].id == QLatin1String(kSrvB) && filtered[2].current);
        }
        // ...and a filtered root whose one server answered NOTHING still offers the way out.
        CHECK(Jellyfin::serverFilterChoices(two, {}, QString::fromLatin1(kSrvB)).size() == 3);
        // One enabled server is still a floor, filter or no filter: the stored id cannot name a second one.
        CHECK(Jellyfin::serverFilterChoices(one, onlyA, QString::fromLatin1(kSrvA)).isEmpty());

        // ---- 21c. CONTINUE WATCHING: MERGED. One section, every item, union order, no server name. ----
        Jellyfin::UnionItem a1; a1.id = qualA; a1.title = QStringLiteral("Alien");
        a1.serverId = QString::fromLatin1(kSrvA); a1.serverName = QStringLiteral("Attic");
        Jellyfin::UnionItem b1; b1.id = qualB; b1.title = QStringLiteral("Brazil");
        b1.serverId = QString::fromLatin1(kSrvB); b1.serverName = QStringLiteral("Loft");
        Jellyfin::UnionItem a2 = a1; a2.title = QStringLiteral("Aliens");
        const QVector<Jellyfin::UnionItem> mixed { a1, b1, a2 };
        const QStringList order { QString::fromLatin1(kSrvA), QString::fromLatin1(kSrvB) };

        const QVector<Jellyfin::ContinueSection> merged =
            Jellyfin::continueSections(mixed, order, /*merged*/ true);
        CHECK(merged.size() == 1);
        if (merged.size() == 1)
        {
            CHECK(merged[0].serverId.isEmpty() && merged[0].serverName.isEmpty());
            CHECK(merged[0].items.size() == 3);
            CHECK(merged[0].items[0].title == QStringLiteral("Alien")
               && merged[0].items[1].title == QStringLiteral("Brazil")
               && merged[0].items[2].title == QStringLiteral("Aliens"));
        }
        // Nothing to show is no section at all, in either shape — the home screen must not grow an empty
        // header (the #161 promise).
        CHECK(Jellyfin::continueSections({}, order, true).isEmpty());
        CHECK(Jellyfin::continueSections({}, order, false).isEmpty());

        // ---- 21d. PER SERVER: the sections, their labels, and their ORDER. ----
        const QVector<Jellyfin::ContinueSection> per =
            Jellyfin::continueSections(mixed, order, /*merged*/ false);
        CHECK(per.size() == 2);
        if (per.size() == 2)
        {
            CHECK(per[0].serverId == QLatin1String(kSrvA));
            CHECK(per[0].serverName == QStringLiteral("Attic"));         // the LABEL
            CHECK(per[0].items.size() == 2);                             // ...and both of that box's rows
            CHECK(per[0].items[0].title == QStringLiteral("Alien")
               && per[0].items[1].title == QStringLiteral("Aliens"));
            CHECK(per[1].serverId == QLatin1String(kSrvB));
            CHECK(per[1].serverName == QStringLiteral("Loft"));
            CHECK(per[1].items.size() == 1 && per[1].items[0].title == QStringLiteral("Brazil"));
        }
        // THE ORDER IS THE STORE'S, NOT THE ITEMS'. Same items, reversed store order, reversed sections —
        // which is what stops the home screen reshuffling according to which box answered first.
        const QStringList reversed { QString::fromLatin1(kSrvB), QString::fromLatin1(kSrvA) };
        const QVector<Jellyfin::ContinueSection> rev =
            Jellyfin::continueSections(mixed, reversed, false);
        CHECK(rev.size() == 2);
        if (rev.size() == 2)
        {
            CHECK(rev[0].serverId == QLatin1String(kSrvB));
            CHECK(rev[1].serverId == QLatin1String(kSrvA));
        }
        // NO DEDUPE, here as everywhere in #160: the same title on two servers is two rows in two sections.
        Jellyfin::UnionItem sameTitleOnB = b1; sameTitleOnB.title = QStringLiteral("Alien");
        const QVector<Jellyfin::ContinueSection> dup =
            Jellyfin::continueSections(QVector<Jellyfin::UnionItem>{ a1, sameTitleOnB }, order, false);
        CHECK(dup.size() == 2);
        if (dup.size() == 2) CHECK(dup[0].items.size() == 1 && dup[1].items.size() == 1);

        // ---- 21e. AN UNREACHABLE SERVER IN PER-SERVER MODE. ----
        // It contributed nothing, so it has NO SECTION — exactly what it costs the home screen today —
        // and the box that did answer keeps its rows. That is the failure isolation, unchanged by the
        // new shape.
        const QVector<Jellyfin::ContinueSection> oneDown =
            Jellyfin::continueSections(QVector<Jellyfin::UnionItem>{ a1, a2 }, order, false);
        CHECK(oneDown.size() == 1);
        if (oneDown.size() == 1)
        {
            CHECK(oneDown[0].serverId == QLatin1String(kSrvA));
            CHECK(oneDown[0].items.size() == 2);
        }
        // ...and a server whose rows are in hand but which is no longer in the store still gets a section,
        // last. Rows are never dropped on the floor because a lookup missed.
        const QVector<Jellyfin::ContinueSection> orphan =
            Jellyfin::continueSections(mixed, QStringList{ QString::fromLatin1(kSrvA) }, false);
        CHECK(orphan.size() == 2);
        if (orphan.size() == 2)
        {
            CHECK(orphan[0].serverId == QLatin1String(kSrvA));
            CHECK(orphan[1].serverId == QLatin1String(kSrvB) && orphan[1].items.size() == 1);
        }
    }

    // =====================================================================================================
    // 22. WHAT IS REMEMBERED, AND WHEN IT RESETS (issue #160, increment 2)
    // =====================================================================================================
    {
        const QString vIni = tmpDir() + QStringLiteral("/view-prefs.ini");
        QFile::remove(vIni);
        JellyfinServerStore::setIniPathForTesting(vIni);

        JellyfinServer sa; sa.id = QString::fromLatin1(kSrvA); sa.name = QStringLiteral("Attic");
        sa.url = QStringLiteral("https://attic.invalid"); sa.userId = QStringLiteral("ua");
        sa.token = QStringLiteral("t-a");
        JellyfinServer sb = sa; sb.id = QString::fromLatin1(kSrvB); sb.name = QStringLiteral("Loft");
        sb.url = QStringLiteral("https://loft.invalid"); sb.userId = QStringLiteral("ub");
        sb.token = QStringLiteral("t-b");
        CHECK(JellyfinServerStore::add(sa));
        CHECK(JellyfinServerStore::add(sb));

        // ---- 22a. The default is ALL SERVERS, and browseServers() is then exactly enabled(). ----
        CHECK(JellyfinServerStore::browseFilterId().isEmpty());
        CHECK(JellyfinServerStore::browseServers().size() == 2);
        CHECK(JellyfinServerStore::continueMerged());          // merged is the default — today's behaviour

        // ---- 22b. The choice is kept, and it NARROWS browseServers() to one. ----
        JellyfinServerStore::setBrowseFilterId(QString::fromLatin1(kSrvB));
        CHECK(JellyfinServerStore::browseFilterId() == QLatin1String(kSrvB));
        CHECK(JellyfinServerStore::browseServers().size() == 1);
        CHECK(JellyfinServerStore::browseServers().value(0).id == QLatin1String(kSrvB));
        // enabled() is UNCHANGED by the filter: the home surfaces still see both servers.
        CHECK(JellyfinServerStore::enabled().size() == 2);

        JellyfinServerStore::setContinueMerged(false);
        CHECK(!JellyfinServerStore::continueMerged());

        // ---- 22c. IT SURVIVES A RESTART. Re-pointing the seam at the same file drops the cached
        // QSettings, so what comes back is what actually reached the disk. ----
        JellyfinServerStore::setIniPathForTesting(QString());
        JellyfinServerStore::setIniPathForTesting(vIni);
        CHECK(JellyfinServerStore::browseFilterId() == QLatin1String(kSrvB));
        CHECK(!JellyfinServerStore::continueMerged());
        CHECK(JellyfinServerStore::browseServers().size() == 1);

        // ---- 22d. BOTH KEYS ARE UNDER THE DEVICE-LOCAL "jellyfin/" PREFIX. ----
        // Read out of the file itself rather than asserted against a constant of this probe's own, so a
        // key that moved out from under the carve-out shows up here. probe_cloudmerge holds the carve-out.
        {
            QSettings raw(vIni, QSettings::IniFormat);
            int jfKeys = 0, otherKeys = 0;
            for (const QString& k : raw.allKeys())
                (k.startsWith(QStringLiteral("jellyfin/")) ? jfKeys : otherKeys)++;
            CHECK(jfKeys >= 3);          // servers, browseFilter, continueMerge
            CHECK(otherKeys == 0);
        }

        // ---- 22e. SWITCHING THE CHOSEN SERVER OFF RESETS IT. ----
        JellyfinServerStore::setEnabled(QString::fromLatin1(kSrvB), false);
        CHECK(JellyfinServerStore::browseFilterId().isEmpty());
        CHECK(JellyfinServerStore::browseServers().size() == 1);       // only A is enabled now
        {
            // Cleared on the DISK, not merely masked on read: switching B back on must not silently put
            // the user back behind a filter they never re-chose.
            QSettings raw(vIni, QSettings::IniFormat);
            bool anyFilterKey = false;
            for (const QString& k : raw.allKeys())
                if (k.endsWith(QStringLiteral("/browseFilter"))) anyFilterKey = true;
            CHECK(!anyFilterKey);
        }
        JellyfinServerStore::setEnabled(QString::fromLatin1(kSrvB), true);
        CHECK(JellyfinServerStore::browseFilterId().isEmpty());

        // ---- 22f. REMOVING THE CHOSEN SERVER RESETS IT TOO. ----
        JellyfinServerStore::setBrowseFilterId(QString::fromLatin1(kSrvB));
        CHECK(JellyfinServerStore::browseFilterId() == QLatin1String(kSrvB));
        JellyfinServerStore::remove(QString::fromLatin1(kSrvB));
        CHECK(JellyfinServerStore::browseFilterId().isEmpty());
        CHECK(JellyfinServerStore::browseServers().size() == 1);

        // ---- 22g. A STORED ID THAT NAMES NO ENABLED SERVER IS MASKED. The belt to 22e/22f's braces —
        // an ini edited by hand, or a row that went away some other way, must not leave the browse root
        // permanently empty. ----
        JellyfinServerStore::setBrowseFilterId(QString::fromLatin1(kSrvB));   // removed above
        CHECK(JellyfinServerStore::browseFilterId().isEmpty());
        CHECK(JellyfinServerStore::browseServers().size() == 1);
        JellyfinServerStore::setBrowseFilterId(QString());

        JellyfinServerStore::setContinueMerged(true);
        CHECK(JellyfinServerStore::continueMerged());
        JellyfinServerStore::setIniPathForTesting(srvIni);
        QFile::remove(vIni);
    }

    // =====================================================================================================
    // 23. A FILTERED BROWSE DOES NOT CALL THE OTHER SERVER (issue #160, increment 2)
    // =====================================================================================================
    // The claim the whole decision rests on, and the only honest way to assert it is to count the requests
    // the other box NEVER RECEIVES. Two fake Jellyfins on 127.0.0.1, the REAL JellyfinClient fan-out, and
    // the real store underneath it.
    {
        FakeJellyfin srvA, srvB;
        CHECK(srvA.listen(QHostAddress::LocalHost, 0));
        CHECK(srvB.listen(QHostAddress::LocalHost, 0));
        srvA.viewsBody = "{\"Items\":[{\"Id\":\"lib-a\",\"Name\":\"A Films\",\"CollectionType\":\"movies\"}]}";
        srvB.viewsBody = "{\"Items\":[{\"Id\":\"lib-b\",\"Name\":\"B Films\",\"CollectionType\":\"movies\"}]}";

        const QString fIni = tmpDir() + QStringLiteral("/fanout.ini");
        QFile::remove(fIni);
        JellyfinServerStore::setIniPathForTesting(fIni);
        JellyfinServer fa;
        fa.id = QString::fromLatin1(kSrvA); fa.name = QStringLiteral("Attic"); fa.url = srvA.root();
        fa.userId = QStringLiteral("ua"); fa.userName = QStringLiteral("p");
        fa.token = QStringLiteral("fixture-a"); fa.allowPlainHttp = true;
        JellyfinServer fb = fa;
        fb.id = QString::fromLatin1(kSrvB); fb.name = QStringLiteral("Loft"); fb.url = srvB.root();
        fb.userId = QStringLiteral("ub"); fb.token = QStringLiteral("fixture-b");
        CHECK(JellyfinServerStore::add(fa));
        CHECK(JellyfinServerStore::add(fb));

        JellyfinClient client;
        auto browse = [&client](QVector<Jellyfin::LibraryRef>* out, QStringList* notes) {
            bool done = false;
            client.fetchLibraries(4000, [&](const QVector<Jellyfin::LibraryRef>& libs, const QStringList& n) {
                if (out) *out = libs;
                if (notes) *notes = n;
                done = true;
            });
            CHECK(waitFor([&] { return done; }));
        };
        auto views = [](const FakeJellyfin& s) {
            int n = 0;
            for (const FakeJellyfin::Seen& seen : s.seen)
                if (seen.path.endsWith(QLatin1String("/Views"))) ++n;
            return n;
        };

        // ---- 23a. UNFILTERED: both boxes are asked, and both libraries come back tagged. ----
        QVector<Jellyfin::LibraryRef> libs;
        QStringList notes;
        browse(&libs, &notes);
        CHECK(views(srvA) == 1);
        CHECK(views(srvB) == 1);
        CHECK(notes.isEmpty());
        CHECK(libs.size() == 2);
        if (libs.size() == 2)
        {
            CHECK(libs[0].serverId == QLatin1String(kSrvA) && libs[0].name == QStringLiteral("A Films"));
            CHECK(libs[1].serverId == QLatin1String(kSrvB) && libs[1].name == QStringLiteral("B Films"));
        }
        // ...and with two contributors the filter is on offer.
        {
            QStringList contributors;
            for (const Jellyfin::LibraryRef& l : libs)
                if (!contributors.contains(l.serverId)) contributors << l.serverId;
            QVector<Jellyfin::ServerChoice> enabledChoices;
            for (const JellyfinServer& s : JellyfinServerStore::enabled())
            {
                Jellyfin::ServerChoice c; c.id = s.id; c.name = s.name; enabledChoices.push_back(c);
            }
            CHECK(Jellyfin::serverFilterChoices(enabledChoices, contributors,
                                                JellyfinServerStore::browseFilterId()).size() == 3);
        }

        // ---- 23b. "SHOW ONLY LOFT": server A IS NOT CALLED AT ALL. ----
        const int aBefore = views(srvA);
        JellyfinServerStore::setBrowseFilterId(QString::fromLatin1(kSrvB));
        browse(&libs, &notes);
        CHECK(views(srvA) == aBefore);        // THE ASSERTION: not one more request reached the other box
        CHECK(views(srvB) == 2);
        CHECK(libs.size() == 1);
        if (libs.size() == 1) CHECK(libs[0].serverId == QLatin1String(kSrvB));
        // ...and no "that server did not answer" note is invented for a server nobody asked.
        CHECK(notes.isEmpty());
        // Settle any straggling sockets before the count below, so it is a claim about the FILTER and not
        // about timing.
        spinFor(250);
        CHECK(views(srvA) == aBefore);

        // ---- 23c. BACK TO ALL SERVERS: A is called again. The filter is a view, not a disconnection. ----
        JellyfinServerStore::setBrowseFilterId(QString());
        browse(&libs, &notes);
        CHECK(views(srvA) == aBefore + 1);
        CHECK(libs.size() == 2);

        // ---- 23d. A FILTER ON A SERVER THAT IS SWITCHED OFF falls back to all servers rather than to an
        // empty root — the reset rule, seen from the fan-out's side. ----
        JellyfinServerStore::setBrowseFilterId(QString::fromLatin1(kSrvB));
        JellyfinServerStore::setEnabled(QString::fromLatin1(kSrvB), false);
        const int aBefore2 = views(srvA), bBefore2 = views(srvB);
        browse(&libs, &notes);
        CHECK(views(srvA) == aBefore2 + 1);
        CHECK(views(srvB) == bBefore2);       // switched off: not asked, filter or no filter
        CHECK(libs.size() == 1);
        if (libs.size() == 1) CHECK(libs[0].serverId == QLatin1String(kSrvA));

        JellyfinServerStore::setIniPathForTesting(srvIni);
        QFile::remove(fIni);
    }

    QFile::remove(ini);
    QFile::remove(srvIni);
    if (failures == 0) std::printf("JELLYFIN-OK\n");
    else               std::printf("JELLYFIN-FAIL %d check(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
