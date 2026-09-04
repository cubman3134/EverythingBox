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
// NO NETWORK. Everything under test is pure or is a QSettings store; the socket half (JellyfinClient) is
// driven live against fixture servers, and the report says so.
//
// NO CREDENTIAL IS EVER PRINTED. The fixture token below is compared, hashed and searched for — never
// written to stdout or stderr, including inside a failing CHECK, which is why the credential sections
// assert on booleans computed beforehand rather than on expressions naming the token.
//
// Prints JELLYFIN-OK on success; any failure prints JELLYFIN-FAIL <cond> and exits non-zero.
#include "Jellyfin.h"
#include "JellyfinMigrate.h"
#include "JellyfinServerStore.h"
#include "RecentStore.h"      // #83: the byte scan below writes a REAL recents row and reads the ini back

#include "AppBrand.h"
#include "AppPaths.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QString>
#include <QStringList>
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

    QFile::remove(ini);
    QFile::remove(srvIni);
    if (failures == 0) std::printf("JELLYFIN-OK\n");
    else               std::printf("JELLYFIN-FAIL %d check(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
