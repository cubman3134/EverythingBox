// probe_subsonic — the Subsonic client's PURE half (issue #193, increment 5), driven with no server, no
// socket and no account.
//
// NO CREDENTIAL APPEARS ANYWHERE IN THIS FILE. Every password below is the literal string
// "probe-not-a-real-password", named so nobody can mistake it for one, and every host is a name that
// resolves nowhere. Nothing here opens a connection - except the last section (#370), which opens one to
// ITSELF: a loopback stub this probe stands up, because what is under test there is what a real reply does.
//
// What is under test, and why each of these and not something easier:
//
//   1. ID QUALIFICATION — the #160 lesson. A round trip that is exact even for a remote id containing the
//      separator; an id from server A that does not resolve against server B; and the structural claim that
//      NO MusicLibrary key can ever parse as a qualified id, driven over keys the REAL buildIndex minted
//      from real tagged files rather than over strings this file made up.
//   2. AUTH — the token is exactly MD5(password + salt); the salt varies per request; the legacy plaintext
//      form is what old servers expect; and an empty password mints no token.
//   3. THE ENVELOPE — the trap that makes naive clients report success: every Subsonic error arrives as
//      HTTP 200 with a failure envelope inside. Plus the claim that the SAME payload read out of the XML
//      and JSON forms produces identical results, which is what lets one set of readers serve both.
//   4. THE BROWSE SHAPES — a server's Index rendered by the very builders #74's local library uses, and the
//      compatibility claim that a local index is unaffected by any of it.
//   5. COVER ANSWERS (#370) — an empty, failed or "not found" cover answer must not re-render the level
//      that asked, so the level cannot ask again in a loop; "no art" is remembered for the session only
//      and holds no credential. Counted in requests the stub actually received, never in time.
#include "Subsonic.h"
#include "AppPaths.h"
#include "Jellyfin.h"
#include "JellyfinMusicClient.h"
#include "JellyfinServerStore.h"
#include "MetaCache.h"
#include "MusicCatalogs.h"
#include "MusicFixtures.h"
#include "MusicLibrary.h"
#include "ServerMusic.h"
#include "ServerMusicClient.h"
#include "SubsonicClient.h"
#include "SubsonicServerStore.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDeadlineTimer>
#include <QDir>
#include <QHostAddress>
#include <QImage>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QUuid>

#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <vector>

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

// Two servers, so every "does this resolve against the other one" question can actually be asked.
static const char* kPassword = "probe-not-a-real-password";

static QString mkServerId() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }

// A small REAL library on disk, written with the shared fixture writers and scanned by the real scanner —
// the same story probe_musicbrowse tells. The claims about local keys and local rendering are only worth
// anything against keys MusicLibrary::buildIndex actually minted.
static bool writeMp3(const QString& path, const QString& title, const QString& artist,
                     const QString& albumArtist, const QString& album,
                     const QString& trck = QString(), const QString& year = QString())
{
    QByteArray frames;
    if (!title.isEmpty())       frames.append(id3TextFrame("TIT2", title));
    if (!artist.isEmpty())      frames.append(id3TextFrame("TPE1", artist));
    if (!albumArtist.isEmpty()) frames.append(id3TextFrame("TPE2", albumArtist));
    if (!album.isEmpty())       frames.append(id3TextFrame("TALB", album));
    if (!trck.isEmpty())        frames.append(id3TextFrame("TRCK", trck));
    if (!year.isEmpty())        frames.append(id3TextFrame("TDRC", year));
    return writeFixture(path, mp3File(frames));
}

static void writeFixtureLibrary(const QString& root)
{
    // An ordinary two-track album...
    writeMp3(root + QStringLiteral("/Boards of Canada/Geogaddi/01.mp3"), QStringLiteral("Ready Lets Go"),
             QStringLiteral("Boards of Canada"), QStringLiteral("Boards of Canada"),
             QStringLiteral("Geogaddi"), QStringLiteral("1/2"), QStringLiteral("2002"));
    writeMp3(root + QStringLiteral("/Boards of Canada/Geogaddi/02.mp3"), QStringLiteral("Music Is Math"),
             QStringLiteral("Boards of Canada"), QStringLiteral("Boards of Canada"),
             QStringLiteral("Geogaddi"), QStringLiteral("2/2"), QStringLiteral("2002"));
    // ...and a COMPILATION, whose per-track artists differ from its album artist. Included because its keys
    // are the awkward ones (the album key is built from "Various Artists", the track artists are not) and
    // the no-collision claim has to hold for those too.
    writeMp3(root + QStringLiteral("/Various/Sampler/01.mp3"), QStringLiteral("One"),
             QStringLiteral("Aardvark"), QStringLiteral("Various Artists"), QStringLiteral("Sampler"),
             QStringLiteral("1/2"), QStringLiteral("1999"));
    writeMp3(root + QStringLiteral("/Various/Sampler/02.mp3"), QStringLiteral("Two"),
             QStringLiteral("Beetle"), QStringLiteral("Various Artists"), QStringLiteral("Sampler"),
             QStringLiteral("2/2"), QStringLiteral("1999"));
    // An UNTAGGED file: its album key is the folder-path form (the "d" discriminator), which is the other
    // local key shape a qualified id must never be confusable with.
    writeMp3(root + QStringLiteral("/Loose/unknown.mp3"), QString(), QString(), QString(), QString());
}

// ==================================================================================================
// 1. Ids
// ==================================================================================================
static void testIds()
{
    const QString A = mkServerId();
    const QString B = mkServerId();

    // --- a round trip is exact ---
    {
        const QString id = Subsonic::qualify(A, Subsonic::Kind::Album, QStringLiteral("al-1234"));
        const Subsonic::Ref r = Subsonic::parse(id);
        CHECK(r.ok);
        CHECK(r.serverId == A);
        CHECK(r.kind == Subsonic::Kind::Album);
        CHECK(r.remoteId == QStringLiteral("al-1234"));
    }

    // --- ...INCLUDING a remote id that contains the separator or a colon. A section()-based reader
    //     truncates both, silently, and the row then plays a different record.
    {
        const QString odd = QStringLiteral("a:b") + Subsonic::idSep() + QStringLiteral("c/d");
        const QString id = Subsonic::qualify(A, Subsonic::Kind::Track, odd);
        const Subsonic::Ref r = Subsonic::parse(id);
        CHECK(r.ok);
        CHECK(r.remoteId == odd);
    }

    // --- an id from server A never resolves against server B ---
    {
        const QString a = Subsonic::qualify(A, Subsonic::Kind::Album, QStringLiteral("1"));
        const QString b = Subsonic::qualify(B, Subsonic::Kind::Album, QStringLiteral("1"));
        // The SAME remote id on two servers — the exact corruption #160 is about.
        CHECK(a != b);
        CHECK(Subsonic::serverOf(a) == A);
        CHECK(Subsonic::serverOf(b) == B);
        // ...and an index built for A cannot answer for B's key, which is the property that matters at the
        // call site rather than mere string inequality.
        Subsonic::RemoteArtist ra; ra.id = QStringLiteral("1"); ra.name = QStringLiteral("One");
        MusicLibrary::Index ia = Subsonic::indexOfArtists(A, { ra });
        CHECK(ia.artist(Subsonic::qualify(A, Subsonic::Kind::Artist, QStringLiteral("1"))) != nullptr);
        CHECK(ia.artist(Subsonic::qualify(B, Subsonic::Kind::Artist, QStringLiteral("1"))) == nullptr);
    }

    // --- half-formed ids are refused rather than half-parsed ---
    CHECK(Subsonic::qualify(QString(), Subsonic::Kind::Album, QStringLiteral("1")).isEmpty());
    CHECK(Subsonic::qualify(A, Subsonic::Kind::Album, QString()).isEmpty());
    CHECK(!Subsonic::isQualified(QString()));
    // A server id that is not a uuid cannot have qualified anything, so it must not parse.
    CHECK(!Subsonic::isQualified(QStringLiteral("sub") + Subsonic::idSep() + QStringLiteral("notauuid")
                                 + Subsonic::idSep() + QStringLiteral("album") + Subsonic::idSep()
                                 + QStringLiteral("1")));
    // An unknown kind word likewise: a stale route must fail closed, not resolve as some other kind. This
    // was spelled "playlist" until increment 6 made that a real kind - which is the hazard the assertion is
    // about, arriving from the other side: a word that is not in the table today may be tomorrow, so the
    // rejection has to be a TABLE LOOKUP rather than a list of the kinds somebody remembered.
    CHECK(!Subsonic::isQualified(QStringLiteral("sub") + Subsonic::idSep() + A + Subsonic::idSep()
                                 + QStringLiteral("sleeve") + Subsonic::idSep() + QStringLiteral("1")));
    // ...and the two kinds increment 6 DID add round-trip like every other one.
    for (Subsonic::Kind k : { Subsonic::Kind::Playlist, Subsonic::Kind::Virtual })
    {
        const QString q = Subsonic::qualify(A, k, QStringLiteral("pl-1"));
        const Subsonic::Ref r = Subsonic::parse(q);
        CHECK(r.ok);
        CHECK(r.kind == k);
        CHECK(r.serverId == A);
        CHECK(r.remoteId == QStringLiteral("pl-1"));
    }
}

// The structural claim, driven over REAL keys: nothing MusicLibrary::buildIndex mints can parse as a
// qualified id. Written against the real scanner and real tagged files rather than invented strings,
// because the whole point is that no artist NAME, however chosen, can produce a collision.
static void testNoLocalKeyParses(const MusicLibrary::Index& local)
{
    CHECK(!local.artists.isEmpty());        // a fixture that scanned nothing would assert nothing below
    for (const MusicLibrary::Artist& a : local.artists)
    {
        CHECK(!Subsonic::isQualified(a.key));
        for (const MusicLibrary::Album& b : a.albums)
        {
            CHECK(!Subsonic::isQualified(b.key));
            for (const MusicLibrary::IndexTrack& t : b.tracks) CHECK(!Subsonic::isQualified(t.path));
        }
        for (const MusicLibrary::IndexTrack& t : a.credits) CHECK(!Subsonic::isQualified(t.path));
    }
    for (const MusicLibrary::Composer& c : local.composers)
    {
        CHECK(!Subsonic::isQualified(c.key));
        for (const MusicLibrary::ComposerWork& w : c.works) CHECK(!Subsonic::isQualified(w.key));
    }
    // ...and the adversarial case the scanner cannot produce on its own: an artist literally called "sub".
    // The second field of an album key is the "t"/"d" discriminator, which is not a uuid, so even this
    // cannot parse. Spelled out because it is the case a reader will ask about.
    const QString hostile = QStringLiteral("sub") + QChar(0x1F) + QStringLiteral("t") + QChar(0x1F)
                          + QStringLiteral("greatest hits");
    CHECK(!Subsonic::isQualified(hostile));
}

// ==================================================================================================
// 2. Auth
// ==================================================================================================
static void testAuth()
{
    const QString pass = QString::fromLatin1(kPassword);

    // --- t is exactly MD5(password + salt), independently computed here ---
    {
        const QString salt = Subsonic::saltFrom(0x0123456789abcdefULL);
        const QString expect = QString::fromLatin1(
            QCryptographicHash::hash((pass + salt).toUtf8(), QCryptographicHash::Md5).toHex());
        CHECK(Subsonic::tokenFor(pass, salt) == expect);
        CHECK(Subsonic::tokenFor(pass, salt).size() == 32);
    }

    // --- a DIFFERENT salt gives a different token: the salt is actually in the hash ---
    {
        const QString s1 = Subsonic::saltFrom(1), s2 = Subsonic::saltFrom(2);
        CHECK(s1 != s2);
        CHECK(Subsonic::tokenFor(pass, s1) != Subsonic::tokenFor(pass, s2));
    }

    // --- the salt VARIES PER REQUEST. Driven through the parameter builder rather than through saltFrom,
    //     because what matters is that two requests do not go out carrying the same one.
    {
        const auto p1 = Subsonic::authParams(QStringLiteral("bob"), pass, Subsonic::saltFrom(11), false,
                                             QStringLiteral("probe"));
        const auto p2 = Subsonic::authParams(QStringLiteral("bob"), pass, Subsonic::saltFrom(22), false,
                                             QStringLiteral("probe"));
        QString s1, s2, t1, t2;
        for (const auto& kv : p1) { if (kv.first == QLatin1String("s")) s1 = kv.second;
                                    if (kv.first == QLatin1String("t")) t1 = kv.second; }
        for (const auto& kv : p2) { if (kv.first == QLatin1String("s")) s2 = kv.second;
                                    if (kv.first == QLatin1String("t")) t2 = kv.second; }
        CHECK(!s1.isEmpty() && !s2.isEmpty());
        CHECK(s1 != s2);
        CHECK(t1 != t2);
        // ...and the PASSWORD is not among the parameters at all under the modern scheme.
        for (const auto& kv : p1) CHECK(kv.second != pass);
    }

    // --- the legacy plaintext form is what an old server expects, and only when asked for ---
    {
        const auto legacy = Subsonic::authParams(QStringLiteral("bob"), pass, Subsonic::saltFrom(3), true,
                                                 QStringLiteral("probe"));
        QString p;
        bool sawToken = false, sawSalt = false;
        for (const auto& kv : legacy)
        {
            if (kv.first == QLatin1String("p")) p = kv.second;
            if (kv.first == QLatin1String("t")) sawToken = true;
            if (kv.first == QLatin1String("s")) sawSalt = true;
        }
        CHECK(p == QStringLiteral("enc:") + QString::fromLatin1(pass.toUtf8().toHex()));
        // The two schemes are exclusive: sending both is Subsonic error 43 (conflicting mechanisms).
        CHECK(!sawToken);
        CHECK(!sawSalt);
    }

    // --- an empty password mints NO token. A well-formed token derived from no password authenticates as
    //     nobody while looking perfectly correct, which is the worst kind of wrong.
    CHECK(Subsonic::tokenFor(QString(), Subsonic::saltFrom(9)).isEmpty());
    CHECK(Subsonic::tokenFor(pass, QString()).isEmpty());

    // --- the STABLE stream salt: same subject, same salt, every time; different subjects differ. This is
    //     what makes a track's stream url its identity, so its resume position can be found again.
    {
        const QString a = Subsonic::stableSalt(QStringLiteral("srv|track-1"));
        const QString b = Subsonic::stableSalt(QStringLiteral("srv|track-1"));
        const QString c = Subsonic::stableSalt(QStringLiteral("srv|track-2"));
        CHECK(a == b);
        CHECK(a != c);
        CHECK(a.size() == 16);
        // ...and it is NOT derived from the password: a salt travels in the clear beside the token, so one
        // computed from the secret would publish a function of it in every url.
        CHECK(!a.contains(Subsonic::tokenFor(pass, a)));
    }
}

// ==================================================================================================
// 3. Transport safety
// ==================================================================================================
static void testUrls()
{
    using V = Subsonic::UrlVerdict;
    CHECK(Subsonic::checkUrl(QStringLiteral("https://music.invalid"), false) == V::Ok);
    // THE ONE THAT MATTERS: plain http is REFUSED unless the user said otherwise, and the refusal is its
    // own verdict rather than a generic failure — a silent downgrade would send the password in clear.
    CHECK(Subsonic::checkUrl(QStringLiteral("http://music.invalid"), false) == V::InsecureRefused);
    CHECK(Subsonic::checkUrl(QStringLiteral("http://music.invalid"), true) == V::Ok);
    CHECK(Subsonic::checkUrl(QStringLiteral("ftp://music.invalid"), true) == V::NotHttp);
    CHECK(Subsonic::checkUrl(QStringLiteral("not a url"), true) == V::Malformed);
    CHECK(Subsonic::checkUrl(QString(), true) == V::Malformed);

    // normalizeRoot trims trailing slashes so every caller can concatenate, and answers EMPTY for a url
    // checkUrl refuses — there is deliberately no fallback to some other server.
    CHECK(Subsonic::normalizeRoot(QStringLiteral("https://m.invalid/"), false)
          == QStringLiteral("https://m.invalid"));
    CHECK(Subsonic::normalizeRoot(QStringLiteral("https://m.invalid///"), false)
          == QStringLiteral("https://m.invalid"));
    CHECK(Subsonic::normalizeRoot(QStringLiteral("http://m.invalid"), false).isEmpty());
}

// ==================================================================================================
// 4. The envelope, in both encodings
// ==================================================================================================
static const char* kOkXml =
    "<?xml version=\"1.0\"?>"
    "<subsonic-response xmlns=\"http://subsonic.org/restapi\" status=\"ok\" version=\"1.16.1\">"
    "<artists><index name=\"A\">"
    "<artist id=\"ar-1\" name=\"Amber\" albumCount=\"2\" coverArt=\"ar-1\"/>"
    "<artist id=\"ar-2\" name=\"Basin\" albumCount=\"1\"/>"
    "</index></artists></subsonic-response>";

static const char* kOkJson =
    "{\"subsonic-response\":{\"status\":\"ok\",\"version\":\"1.16.1\",\"artists\":{\"index\":[{\"name\":\"A\","
    "\"artist\":[{\"id\":\"ar-1\",\"name\":\"Amber\",\"albumCount\":2,\"coverArt\":\"ar-1\"},"
    "{\"id\":\"ar-2\",\"name\":\"Basin\",\"albumCount\":1}]}]}}}";

static const char* kFailXml =
    "<subsonic-response status=\"failed\" version=\"1.16.1\">"
    "<error code=\"40\" message=\"Wrong username or password.\"/></subsonic-response>";

static const char* kFailJson =
    "{\"subsonic-response\":{\"status\":\"failed\",\"version\":\"1.16.1\","
    "\"error\":{\"code\":40,\"message\":\"Wrong username or password.\"}}}";

static void testEnvelope()
{
    // --- an OK envelope, in both encodings ---
    for (const char* body : { kOkXml, kOkJson })
    {
        bool ok = false;
        const Subsonic::Node root = Subsonic::parseBody(QByteArray(body), &ok);
        CHECK(ok);
        const Subsonic::Envelope e = Subsonic::envelopeOf(root);
        CHECK(e.status == Subsonic::Status::Ok);
        CHECK(e.ok());
        CHECK(e.version == QStringLiteral("1.16.1"));
        CHECK(e.code == 0);
    }

    // --- THE TRAP: a failure that arrives as HTTP 200. Nothing here can see the HTTP status, which is the
    //     point — a client that decided on the status alone would call both of these a success.
    for (const char* body : { kFailXml, kFailJson })
    {
        bool ok = false;
        const Subsonic::Node root = Subsonic::parseBody(QByteArray(body), &ok);
        CHECK(ok);                                   // it PARSED: the body is perfectly well-formed
        const Subsonic::Envelope e = Subsonic::envelopeOf(root);
        CHECK(e.status == Subsonic::Status::Failed); // ...and it is still a failure
        CHECK(!e.ok());
        CHECK(e.code == 40);
        CHECK(e.message == QStringLiteral("Wrong username or password."));
        CHECK(Subsonic::isAuthCode(e.code));         // a refused credential: retrying changes nothing
    }
    CHECK(!Subsonic::isAuthCode(70));                // "not found" is not an auth problem

    // --- a subsonic-response that does not SAY it succeeded did not succeed. "No status attribute" must
    //     not read as ok, or the 200-means-fine bug walks back in through the missing-field door.
    {
        bool ok = false;
        const Subsonic::Node root = Subsonic::parseBody(
            QByteArray("<subsonic-response version=\"1.16.1\"/>"), &ok);
        CHECK(ok);
        CHECK(Subsonic::envelopeOf(root).status == Subsonic::Status::Failed);
    }

    // --- not a subsonic-response at all: a reverse proxy's HTML page, a captive portal, plain garbage ---
    for (const char* body : { "<html><body>502 Bad Gateway</body></html>", "not json or xml", "" })
    {
        bool ok = false;
        const Subsonic::Node root = Subsonic::parseBody(QByteArray(body), &ok);
        CHECK(Subsonic::envelopeOf(root).status == Subsonic::Status::Unparsable);
    }
}

// The claim that lets ONE set of payload readers serve both encodings: the same answer, read out of the XML
// and the JSON form of the same reply, is identical.
static void testBothEncodingsAgree()
{
    bool okX = false, okJ = false;
    const Subsonic::Node x = Subsonic::parseBody(QByteArray(kOkXml), &okX);
    const Subsonic::Node j = Subsonic::parseBody(QByteArray(kOkJson), &okJ);
    CHECK(okX && okJ);

    const QVector<Subsonic::RemoteArtist> ax = Subsonic::readArtists(x);
    const QVector<Subsonic::RemoteArtist> aj = Subsonic::readArtists(j);
    CHECK(ax.size() == 2);
    CHECK(ax.size() == aj.size());
    for (int i = 0; i < ax.size() && i < aj.size(); ++i)
    {
        CHECK(ax[i].id == aj[i].id);
        CHECK(ax[i].name == aj[i].name);
        // The typed/untyped difference the node model exists to flatten: 2 in JSON, "2" in XML.
        CHECK(ax[i].albumCount == aj[i].albumCount);
        CHECK(ax[i].coverArt == aj[i].coverArt);
    }
    CHECK(ax[0].albumCount == 2);
    CHECK(ax[1].albumCount == 1);
}

static void testPayloads()
{
    // getArtist: an artist's albums, each with the server's own song count.
    const char* albumsJson =
        "{\"subsonic-response\":{\"status\":\"ok\",\"artist\":{\"id\":\"ar-1\",\"name\":\"Amber\","
        "\"album\":[{\"id\":\"al-9\",\"name\":\"Tideline\",\"artist\":\"Amber\",\"artistId\":\"ar-1\","
        "\"songCount\":11,\"year\":2019,\"duration\":2640,\"coverArt\":\"al-9\"}]}}}";
    bool ok = false;
    const Subsonic::Node r = Subsonic::parseBody(QByteArray(albumsJson), &ok);
    CHECK(ok);
    const QVector<Subsonic::RemoteAlbum> albums = Subsonic::readAlbums(r);
    CHECK(albums.size() == 1);
    CHECK(albums[0].id == QStringLiteral("al-9"));
    CHECK(albums[0].name == QStringLiteral("Tideline"));
    CHECK(albums[0].songCount == 11);
    CHECK(albums[0].year == 2019);

    // getAlbum: songs, in the shape a track row needs.
    const char* songsXml =
        "<subsonic-response status=\"ok\"><album id=\"al-9\" name=\"Tideline\" coverArt=\"al-9\">"
        "<song id=\"s-2\" title=\"Second\" artist=\"Amber\" album=\"Tideline\" track=\"2\" duration=\"200\"/>"
        "<song id=\"s-1\" title=\"First\" artist=\"Amber\" album=\"Tideline\" track=\"1\" duration=\"180\"/>"
        "</album></subsonic-response>";
    ok = false;
    const Subsonic::Node rs = Subsonic::parseBody(QByteArray(songsXml), &ok);
    CHECK(ok);
    const QVector<Subsonic::RemoteSong> songs = Subsonic::readSongs(rs);
    CHECK(songs.size() == 2);
    CHECK(songs[0].id == QStringLiteral("s-2"));      // read in document order; the SORT happens on the way
                                                      // into the index, not here
    // A getMusicDirectory-style reply: a folder row must never reach a queue as if it were audio.
    const char* dirXml =
        "<subsonic-response status=\"ok\"><directory id=\"d\">"
        "<child id=\"c-1\" isDir=\"true\" title=\"Disc 1\"/>"
        "<child id=\"c-2\" title=\"Real Song\" duration=\"120\"/>"
        "</directory></subsonic-response>";
    ok = false;
    const QVector<Subsonic::RemoteSong> kids = Subsonic::readSongs(Subsonic::parseBody(QByteArray(dirXml), &ok));
    CHECK(ok);
    CHECK(kids.size() == 1);
    CHECK(kids[0].id == QStringLiteral("c-2"));
}

// ==================================================================================================
// 5. Onto the existing catalog shapes
// ==================================================================================================
static void testIndexShapes()
{
    const QString S = mkServerId();

    Subsonic::RemoteArtist ra; ra.id = QStringLiteral("ar-1"); ra.name = QStringLiteral("Amber");
    ra.albumCount = 3;
    MusicLibrary::Index idx = Subsonic::indexOfArtists(S, { ra });
    CHECK(idx.artists.size() == 1);
    const QString artistKey = Subsonic::qualify(S, Subsonic::Kind::Artist, QStringLiteral("ar-1"));
    CHECK(idx.artists[0].key == artistKey);
    CHECK(idx.artists[0].albumCount == 3);           // known from the listing...
    CHECK(idx.artists[0].albums.isEmpty());          // ...while the albums themselves are not fetched yet
    // Index::trackCount stays 0 ON PURPOSE: it gates "Shuffle all music", which over unfetched tracks could
    // only ever produce an empty queue. An honest absence, not a wrong number.
    CHECK(idx.trackCount == 0);

    // The artists level renders through the SAME builder the local library uses, and says "3 albums"
    // WITHOUT inventing a track count it does not have.
    {
        const MediaCatalog cat = browse::musicArtistsCatalog(idx, {}, {}, /*musicServerCount*/ 0);
        CHECK(cat.items.size() == 1);
        CHECK(cat.items[0].type == QString::fromLatin1(browse::kMusicArtistType));
        CHECK(cat.items[0].title == QStringLiteral("Amber"));
        CHECK(cat.items[0].subtitle.contains(QStringLiteral("3")));
        // THE CLAUSE THAT MUST BE ABSENT. "0 tracks" beside "3 albums" is a number this app made up.
        CHECK(!cat.items[0].subtitle.contains(QStringLiteral("0 track")));
    }

    // Drill the artist: albums arrive with the server's song counts, tracks still absent.
    Subsonic::RemoteAlbum rb;
    rb.id = QStringLiteral("al-9"); rb.name = QStringLiteral("Tideline"); rb.artist = QStringLiteral("Amber");
    rb.songCount = 2; rb.year = 2019; rb.coverArt = QStringLiteral("al-9");
    Subsonic::fillArtistAlbums(idx, S, artistKey, { rb });
    const QString albumKey = Subsonic::qualify(S, Subsonic::Kind::Album, QStringLiteral("al-9"));
    const MusicLibrary::Album* b = idx.album(albumKey);
    CHECK(b != nullptr);
    if (b)
    {
        CHECK(b->trackCount == 2);                   // the server's count...
        CHECK(b->tracks.isEmpty());                  // ...before the tracks are fetched
    }
    {
        const MediaCatalog cat = browse::musicArtistCatalog(idx, artistKey);
        // THREE rows: "Play all", "Shuffle all", then the album (issue #194, increment 2).
        //
        // This assertion used to say ONE row, on the reasoning that Artist::trackCount is 0 for a remote
        // artist and queueing tracks nobody has fetched produces an empty queue. The second half of that is
        // still true and is now somebody else's job: the verb FETCHES the track lists it is missing before
        // it plays (HomeView::playMusicArtistQueue), exactly as the "Play from <supplier>" row already does
        // for one record. Withholding the rows instead cost a server-backed artist the only two multi-album
        // queues this app can build — and with them crossfade and ReplayGain's track mode, which have no
        // boundary to work on inside a single record.
        //
        // The gate is the REACHABLE count — what the server said each album holds — not what has been
        // fetched. This is the assertion that goes red if it reverts to `tracks.size()` or to
        // Artist::trackCount, both of which are 0 here.
        CHECK(cat.items.size() == 3);
        CHECK(cat.items[0].type == QString::fromLatin1(browse::kMusicPlayArtistType));
        CHECK(cat.items[1].type == QString::fromLatin1(browse::kMusicShuffleArtistType));
        CHECK(cat.items[2].type == QString::fromLatin1(browse::kMusicAlbumType));
        CHECK(cat.items[0].subtitle.contains(QStringLiteral("2 track")));
        // ...and the album row's own subtitle is still the SERVER's count, read off Album::trackCount
        // rather than off an empty `tracks` vector.
        CHECK(cat.items[2].subtitle.contains(QStringLiteral("2 track")));
    }

    // Drill the album: tracks arrive, ordered disc-then-track exactly as a local album is.
    Subsonic::RemoteSong s2; s2.id = QStringLiteral("s-2"); s2.title = QStringLiteral("Second");
    s2.artist = QStringLiteral("Amber"); s2.track = 2; s2.durationSec = 200;
    Subsonic::RemoteSong s1; s1.id = QStringLiteral("s-1"); s1.title = QStringLiteral("First");
    s1.artist = QStringLiteral("Amber"); s1.track = 1; s1.durationSec = 180;
    Subsonic::fillAlbumTracks(idx, S, albumKey, { s2, s1 });   // deliberately out of order
    b = idx.album(albumKey);
    CHECK(b != nullptr);
    if (b)
    {
        CHECK(b->tracks.size() == 2);
        CHECK(b->tracks[0].title == QStringLiteral("First"));   // sorted, like a local album
        CHECK(b->tracks[1].title == QStringLiteral("Second"));
        CHECK(b->durationSec == 380);
        // WHAT THE INDEX STORES IS AN ID, NOT A URL. A stream url carries the token and the salt, and this
        // struct is copied into queues and into anything that persists one.
        CHECK(Subsonic::isQualified(b->tracks[0].path));
        CHECK(Subsonic::parse(b->tracks[0].path).kind == Subsonic::Kind::Track);
        CHECK(!b->tracks[0].path.contains(QStringLiteral("http")));
    }
    {
        const MediaCatalog cat = browse::musicAlbumCatalog(idx, albumKey);
        CHECK(cat.items.size() == 3);                // "Play album" + two tracks
        CHECK(cat.items[0].type == QString::fromLatin1(browse::kMusicPlayAlbumType));
        CHECK(cat.items[1].type == QString::fromLatin1(browse::kMusicTrackType));
        // The track row's mime carries the ALBUM key, which is what routes it to the album queue — the same
        // contract a local track row has, read back through the same reader.
        CHECK(browse::musicKeyOf(cat.items[1].mime, browse::kMusicTrackPrefix) == albumKey);
    }

    // A STALE ROUTE renders an empty level rather than crashing or playing something else.
    const QString otherServerAlbum = Subsonic::qualify(mkServerId(), Subsonic::Kind::Album,
                                                       QStringLiteral("al-9"));
    CHECK(idx.album(otherServerAlbum) == nullptr);
    CHECK(browse::musicAlbumCatalog(idx, otherServerAlbum).items.isEmpty());
}

// THE COLD CACHE. The per-server index lives for the session, so anything that REMEMBERS an album across
// runs — a Recents row, and later a favourite or a saved queue — names a record whose artist has never been
// fetched. adoptAlbum is what turns that name back into a playable record; without it the remembered row is
// silently dead, which is the failure this codebase treats as worse than an error.
static void testColdCacheAdopt()
{
    const QString S = mkServerId();
    MusicLibrary::Index idx;                       // nothing has been fetched at all
    CHECK(idx.artists.isEmpty());

    Subsonic::RemoteAlbum b;
    b.id = QStringLiteral("al-9"); b.name = QStringLiteral("Tideline");
    b.artist = QStringLiteral("Amber"); b.artistId = QStringLiteral("ar-1");
    b.songCount = 2; b.year = 2019;
    Subsonic::RemoteSong s1; s1.id = QStringLiteral("s-1"); s1.title = QStringLiteral("First");
    s1.track = 1; s1.durationSec = 180;
    Subsonic::RemoteSong s2; s2.id = QStringLiteral("s-2"); s2.title = QStringLiteral("Second");
    s2.track = 2; s2.durationSec = 200;

    Subsonic::adoptAlbum(idx, S, b, { s1, s2 });
    const QString albumKey = Subsonic::qualify(S, Subsonic::Kind::Album, QStringLiteral("al-9"));
    const MusicLibrary::Album* got = idx.album(albumKey);
    CHECK(got != nullptr);
    if (got)
    {
        CHECK(got->tracks.size() == 2);            // the record is playable, which is the whole point
        CHECK(got->title == QStringLiteral("Tideline"));
        CHECK(got->albumArtist == QStringLiteral("Amber"));
    }
    // It hangs off the server's OWN artist, so a later getArtist for that artist lands on the same bucket
    // rather than producing a second copy of the same person.
    CHECK(idx.artists.size() == 1);
    CHECK(idx.artists[0].key == Subsonic::qualify(S, Subsonic::Kind::Artist, QStringLiteral("ar-1")));

    // Adopting the SAME album twice does not duplicate it — the Recents row re-opened after a browse must
    // land on the record already there.
    Subsonic::adoptAlbum(idx, S, b, { s1, s2 });
    CHECK(idx.artists.size() == 1);
    CHECK(idx.artists[0].albums.size() == 1);

    // A server that gives no artistId still yields ONE bucket per record rather than merging unrelated
    // albums under an empty key.
    Subsonic::RemoteAlbum c = b;
    c.id = QStringLiteral("al-10"); c.name = QStringLiteral("Second Wind"); c.artistId.clear();
    Subsonic::adoptAlbum(idx, S, c, { s1 });
    CHECK(idx.album(Subsonic::qualify(S, Subsonic::Kind::Album, QStringLiteral("al-10"))) != nullptr);
    CHECK(idx.artists.size() == 2);
}

// The "Music Servers" door and the level behind it.
static void testServersLevel()
{
    // With no servers the door is ABSENT — the compatibility claim the whole increment rests on.
    Subsonic::RemoteArtist ra; ra.id = QStringLiteral("ar-1"); ra.name = QStringLiteral("Amber");
    const MusicLibrary::Index idx = Subsonic::indexOfArtists(mkServerId(), { ra });
    const MediaCatalog none = browse::musicArtistsCatalog(idx, {}, {}, 0);
    for (const MediaItem& it : none.items)
        CHECK(it.type != QString::fromLatin1(browse::kMusicServersType));

    const MediaCatalog some = browse::musicArtistsCatalog(idx, {}, {}, 2);
    bool sawDoor = false;
    for (const MediaItem& it : some.items)
        if (it.type == QString::fromLatin1(browse::kMusicServersType)) sawDoor = true;
    CHECK(sawDoor);
    CHECK(some.items.size() == none.items.size() + 1);   // ONE row, nothing else moved

    // An EMPTY music root with a server configured shows the door and no "choose a folder" sentence — the
    // level a Navidrome-only user lands on.
    {
        const MusicLibrary::Index empty;
        const MediaCatalog cat = browse::musicArtistsCatalog(empty, {}, {}, 1);
        CHECK(cat.items.size() == 1);
        CHECK(cat.items[0].type == QString::fromLatin1(browse::kMusicServersType));
    }

    // The servers level itself: one row per server, plus the trailing add row that is the whole level when
    // nothing is saved yet.
    {
        const MediaCatalog cat = browse::musicServersCatalog({}, {}, {});
        CHECK(cat.items.size() == 1);
        CHECK(cat.items[0].type == QString::fromLatin1(browse::kMusicAddServerType));
    }
    {
        const QString id = mkServerId();
        const MediaCatalog cat = browse::musicServersCatalog({ id }, { QStringLiteral("Basement") },
                                                             { QStringLiteral("https://m.invalid") });
        CHECK(cat.items.size() == 2);
        CHECK(cat.items[0].type == QString::fromLatin1(browse::kMusicServerType));
        CHECK(cat.items[0].title == QStringLiteral("Basement"));
        CHECK(browse::musicKeyOf(cat.items[0].mime, browse::kMusicServerPrefix) == id);
        CHECK(cat.items[1].type == QString::fromLatin1(browse::kMusicAddServerType));
    }
}

// ==================================================================================================
// 6. THE COMPATIBILITY CLAIM — a local library is what it was
// ==================================================================================================
// Guardrail, not decoration: this increment touched the two subtitle reads and added two count fields to
// structs #74 owns. The claim is that a scanned library renders identically, and it is CHECKED rather than
// argued — over an index the real scanner built from real tagged files.
static void testLocalUnchanged(const MusicLibrary::Index& local)
{
    for (const MusicLibrary::Artist& a : local.artists)
    {
        // The two new fields ARE the container sizes for a scanned library, by construction in buildIndex.
        // If this ever drifts, every browse subtitle that now reads them is wrong.
        CHECK(a.albumCount == int(a.albums.size()));
        for (const MusicLibrary::Album& b : a.albums) CHECK(b.trackCount == int(b.tracks.size()));
        // ...and the track clause that is omitted when the count is zero can never be omitted locally: an
        // artist bucket is minted BY a track, so the sum is at least one for every artist that exists.
        CHECK(a.trackCount + int(a.credits.size()) > 0);
    }
    // The Music root with no servers is byte-for-byte the catalog it was: same row count, same types, same
    // titles, same subtitles as the default-argument call this feature did not exist for.
    const browse::MusicEmptyNote note;
    const MediaCatalog before = browse::musicArtistsCatalog(local, note);        // the pre-#193 call shape
    const MediaCatalog after  = browse::musicArtistsCatalog(local, note, {}, 0); // ...and the new one
    CHECK(before.items.size() == after.items.size());
    CHECK(before.title == after.title);
    for (int i = 0; i < before.items.size() && i < after.items.size(); ++i)
    {
        CHECK(before.items[i].type == after.items[i].type);
        CHECK(before.items[i].id == after.items[i].id);
        CHECK(before.items[i].title == after.items[i].title);
        CHECK(before.items[i].subtitle == after.items[i].subtitle);
        CHECK(before.items[i].mime == after.items[i].mime);
    }
    // ...and none of those rows is a servers door.
    for (const MediaItem& it : after.items)
        CHECK(it.type != QString::fromLatin1(browse::kMusicServersType));
}


// ==================================================================================================
// 9. WHAT THE SERVER ALREADY KNOWS (issue #193, increment 6) - playlists, starred, recently added
// ==================================================================================================
//
// Every payload here is driven in BOTH encodings wherever the reader is shared, for the reason section 4
// gives: `f=json` is a request rather than a guarantee, and a reader that could only see one of them would
// be a silent no-op against half the deployments in the wild.

static const char* kPlaylistsXml =
    "<subsonic-response status=\"ok\" version=\"1.16.1\"><playlists>"
    "<playlist id=\"pl-1\" name=\"Late night\" owner=\"ada\" songCount=\"3\" duration=\"600\" "
    "coverArt=\"pl-1\"/>"
    "<playlist id=\"pl-2\" name=\"Empty\" owner=\"ada\" songCount=\"0\" duration=\"0\"/>"
    // MALFORMED: no id at all. A row that cannot be opened is worse than an absent one, so it is dropped.
    "<playlist name=\"Nameless and idless\" songCount=\"9\"/>"
    "</playlists></subsonic-response>";

static const char* kPlaylistsJson =
    "{\"subsonic-response\":{\"status\":\"ok\",\"version\":\"1.16.1\",\"playlists\":{\"playlist\":["
    "{\"id\":\"pl-1\",\"name\":\"Late night\",\"owner\":\"ada\",\"songCount\":3,\"duration\":600,"
    "\"coverArt\":\"pl-1\"},"
    "{\"id\":\"pl-2\",\"name\":\"Empty\",\"owner\":\"ada\",\"songCount\":0,\"duration\":0},"
    "{\"name\":\"Nameless and idless\",\"songCount\":9}]}}}";

// getPlaylist. Its tracks are <entry>, NOT <song> - a third spelling of the same element, and the one a
// reader written against getAlbum alone has never seen. The track NUMBERS here are deliberately out of
// order relative to the listing: these songs came off three different records.
static const char* kPlaylistXml =
    "<subsonic-response status=\"ok\" version=\"1.16.1\">"
    "<playlist id=\"pl-1\" name=\"Late night\" owner=\"ada\" songCount=\"3\" duration=\"600\">"
    "<entry id=\"s-9\" title=\"Third on its record\" artist=\"Amber\" album=\"Dusk\" albumId=\"al-1\" "
    "track=\"3\" duration=\"200\"/>"
    "<entry id=\"s-1\" title=\"First on its record\" artist=\"Basin\" album=\"Reeds\" albumId=\"al-2\" "
    "track=\"1\" duration=\"180\"/>"
    "<entry id=\"s-5\" title=\"Fifth on its record\" artist=\"Cedar\" album=\"Bark\" albumId=\"al-3\" "
    "track=\"5\" duration=\"220\"/>"
    "</playlist></subsonic-response>";

static const char* kStarredXml =
    "<subsonic-response status=\"ok\" version=\"1.16.1\"><starred2>"
    "<artist id=\"ar-7\" name=\"Delta\" albumCount=\"4\"/>"
    "<album id=\"al-8\" name=\"Loved record\" artist=\"Echo\" artistId=\"ar-8\" songCount=\"11\" "
    "year=\"2011\" coverArt=\"al-8\"/>"
    "<song id=\"s-11\" title=\"Loved track\" artist=\"Foxglove\" album=\"Somewhere\" albumId=\"al-9\" "
    "track=\"2\" duration=\"240\"/>"
    "<song id=\"s-12\" title=\"Another loved track\" artist=\"Grove\" album=\"Elsewhere\" "
    "albumId=\"al-10\" track=\"7\" duration=\"190\"/>"
    "</starred2></subsonic-response>";

static const char* kStarredEmptyXml =
    "<subsonic-response status=\"ok\" version=\"1.16.1\"><starred2/></subsonic-response>";

static const char* kNewestXml =
    "<subsonic-response status=\"ok\" version=\"1.16.1\"><albumList2>"
    "<album id=\"al-20\" name=\"Just arrived\" artist=\"Hazel\" artistId=\"ar-20\" songCount=\"9\" "
    "year=\"2024\" duration=\"2400\" coverArt=\"al-20\"/>"
    // MALFORMED: no id. Dropped, for the same reason the idless playlist is.
    "<album name=\"No id here\" artist=\"Nobody\" songCount=\"4\"/>"
    "</albumList2></subsonic-response>";

static Subsonic::Node parsedOk(const char* body)
{
    bool ok = false;
    const Subsonic::Node n = Subsonic::parseBody(QByteArray(body), &ok);
    CHECK(ok);
    return n;
}

static void testPlaylistPayload()
{
    const QString A = mkServerId();

    // --- the LIST, in both encodings, producing identical results ---
    const QVector<Subsonic::RemotePlaylist> fromXml = Subsonic::readPlaylists(parsedOk(kPlaylistsXml));
    const QVector<Subsonic::RemotePlaylist> fromJson = Subsonic::readPlaylists(parsedOk(kPlaylistsJson));
    CHECK(fromXml.size() == 2);                 // the idless row is gone from BOTH
    CHECK(fromJson.size() == 2);
    for (int i = 0; i < fromXml.size() && i < fromJson.size(); ++i)
    {
        CHECK(fromXml[i].id == fromJson[i].id);
        CHECK(fromXml[i].name == fromJson[i].name);
        CHECK(fromXml[i].songCount == fromJson[i].songCount);
        CHECK(fromXml[i].owner == fromJson[i].owner);
    }
    CHECK(fromXml.at(0).id == QStringLiteral("pl-1"));
    CHECK(fromXml.at(0).songCount == 3);
    CHECK(fromXml.at(0).coverArt == QStringLiteral("pl-1"));
    // A PLAYLIST WITH NO TRACKS IS STILL A PLAYLIST. It has to survive the read, or a level somebody made
    // and has not filled yet simply vanishes.
    CHECK(fromXml.at(1).id == QStringLiteral("pl-2"));
    CHECK(fromXml.at(1).songCount == 0);

    // --- an EMPTY result is empty rather than a failure ---
    CHECK(Subsonic::readPlaylists(parsedOk(
              "<subsonic-response status=\"ok\" version=\"1.16.1\"><playlists/></subsonic-response>"))
              .isEmpty());

    // --- the rows the level draws ---
    const QVector<MusicLibrary::Album> rows = Subsonic::playlistRows(A, fromXml);
    CHECK(rows.size() == 2);
    CHECK(rows.at(0).key == Subsonic::qualify(A, Subsonic::Kind::Playlist, QStringLiteral("pl-1")));
    CHECK(rows.at(0).title == QStringLiteral("Late night"));
    // THE SERVER'S OWN COUNT, before any track has been fetched - the split MusicLibrary::Album::trackCount
    // exists for. tracks.size() is 0 here and would print "0 tracks" beside a record holding three.
    CHECK(rows.at(0).trackCount == 3);
    CHECK(rows.at(0).tracks.isEmpty());
    // ...and the id says PLAYLIST, which is what routes the fetch to getPlaylist rather than getAlbum.
    CHECK(Subsonic::parse(rows.at(0).key).kind == Subsonic::Kind::Playlist);
}

static void testPlaylistTracksKeepTheirOrder()
{
    const QString A = mkServerId();
    const Subsonic::Node root = parsedOk(kPlaylistXml);
    const QVector<Subsonic::RemotePlaylist> info = Subsonic::readPlaylists(root);
    CHECK(info.size() == 1);
    if (info.isEmpty()) return;

    // <entry>, the playlist spelling. readSongs has to see it or a playlist opens on nothing at all.
    const QVector<Subsonic::RemoteSong> songs = Subsonic::readSongs(root);
    CHECK(songs.size() == 3);
    if (songs.size() != 3) return;
    CHECK(songs.at(0).id == QStringLiteral("s-9"));

    MusicLibrary::Index sections;
    Subsonic::adoptPlaylist(sections, A, info.first(), songs);
    const QString key = Subsonic::qualify(A, Subsonic::Kind::Playlist, QStringLiteral("pl-1"));
    const MusicLibrary::Album* b = sections.album(key);
    CHECK(b != nullptr);
    if (!b) return;
    CHECK(b->title == QStringLiteral("Late night"));
    CHECK(b->tracks.size() == 3);
    CHECK(b->trackCount == 3);
    if (b->tracks.size() != 3) return;

    // THE CLAIM THIS TEST EXISTS FOR. The order somebody arranged is the entire content of a playlist, and
    // the album level's disc-then-track sort would rewrite it into 1, 3, 5 - an order nobody chose and no
    // server would agree with. It must come out exactly as the server listed it.
    CHECK(b->tracks.at(0).path == Subsonic::qualify(A, Subsonic::Kind::Track, QStringLiteral("s-9")));
    CHECK(b->tracks.at(1).path == Subsonic::qualify(A, Subsonic::Kind::Track, QStringLiteral("s-1")));
    CHECK(b->tracks.at(2).path == Subsonic::qualify(A, Subsonic::Kind::Track, QStringLiteral("s-5")));
    // ...and every row is queued behind the PLAYLIST, not behind the record the song is also on.
    for (const MusicLibrary::IndexTrack& t : b->tracks) CHECK(t.albumKey == key);
    // AN ID, NEVER A URL. The index is copied into queues and a queue is persisted; a stream url here would
    // write the user's token onto disk. SubsonicClient.h states the rule; this asserts it.
    for (const MusicLibrary::IndexTrack& t : b->tracks)
    {
        CHECK(Subsonic::parse(t.path).kind == Subsonic::Kind::Track);
        CHECK(!t.path.contains(QStringLiteral("http")));
        CHECK(!t.path.contains(QLatin1Char('&')));
    }

    // A PLAYLIST WITH NO TRACKS adopts as an empty record rather than not at all - the level has to say
    // "this is empty", not "this does not exist".
    MusicLibrary::Index none;
    Subsonic::RemotePlaylist p;
    p.id = QStringLiteral("pl-2"); p.name = QStringLiteral("Empty");
    Subsonic::adoptPlaylist(none, A, p, {});
    const MusicLibrary::Album* e = none.album(Subsonic::qualify(A, Subsonic::Kind::Playlist, p.id));
    CHECK(e != nullptr);
    if (e) { CHECK(e->tracks.isEmpty()); CHECK(e->trackCount == 0); }

    // IDEMPOTENT: re-running the adopt replaces the tracks rather than doubling them. Not hypothetical -
    // the level re-fetches on Back.
    Subsonic::adoptPlaylist(sections, A, info.first(), songs);
    const MusicLibrary::Album* again = sections.album(key);
    CHECK(again != nullptr);
    if (again) CHECK(again->tracks.size() == 3);
}

static void testStarredPayload()
{
    const QString A = mkServerId();
    const Subsonic::Starred s = Subsonic::readStarred(A, parsedOk(kStarredXml));
    CHECK(s.artists.size() == 1);
    CHECK(s.albums.size() == 1);
    CHECK(s.tracks.size() == 2);
    CHECK(!s.isEmpty());
    if (s.artists.size() != 1 || s.albums.size() != 1 || s.tracks.size() != 2) return;

    // REAL ids for the artist and the album, so opening one takes the ordinary route and the ordinary fetch.
    CHECK(s.artists.at(0).key == Subsonic::qualify(A, Subsonic::Kind::Artist, QStringLiteral("ar-7")));
    CHECK(s.artists.at(0).albumCount == 4);
    CHECK(s.artists.at(0).trackCount == 0);       // deliberate: the server told us albums and nothing else
    CHECK(s.albums.at(0).key == Subsonic::qualify(A, Subsonic::Kind::Album, QStringLiteral("al-8")));
    CHECK(s.albums.at(0).trackCount == 11);

    // ...and the loose tracks are queued behind the ONE INVENTED RECORD, so pressing the second one plays
    // the starred list from there. A track row with no resolvable album key would play nothing at all.
    const QString virt = Subsonic::starredTracksKey(A);
    CHECK(Subsonic::parse(virt).kind == Subsonic::Kind::Virtual);
    for (const MusicLibrary::IndexTrack& t : s.tracks) CHECK(t.albumKey == virt);
    CHECK(s.tracks.at(0).title == QStringLiteral("Loved track"));

    MusicLibrary::Index sections;
    Subsonic::adoptStarred(sections, A, s);
    const MusicLibrary::Album* b = sections.album(virt);
    CHECK(b != nullptr);
    if (b) CHECK(b->tracks.size() == 2);
    // IDEMPOTENT, and this one is load-bearing: the level re-adopts whenever it is re-read, and an append
    // would show every starred track twice the second time somebody opened it.
    Subsonic::adoptStarred(sections, A, s);
    const MusicLibrary::Album* again = sections.album(virt);
    CHECK(again != nullptr);
    if (again) CHECK(again->tracks.size() == 2);

    // AN EMPTY STARRED LIST is empty, not a failure, and invents no record for nothing.
    const Subsonic::Starred none = Subsonic::readStarred(A, parsedOk(kStarredEmptyXml));
    CHECK(none.isEmpty());
    MusicLibrary::Index empty;
    Subsonic::adoptStarred(empty, A, none);
    CHECK(empty.artists.isEmpty());

    // A MALFORMED record - a song with no id - never becomes a row that cannot be played.
    const Subsonic::Starred bad = Subsonic::readStarred(A, parsedOk(
        "<subsonic-response status=\"ok\" version=\"1.16.1\"><starred2>"
        "<song title=\"No id\" artist=\"Nobody\"/></starred2></subsonic-response>"));
    CHECK(bad.tracks.isEmpty());
}

static void testNewestPayload()
{
    const QString A = mkServerId();
    const QVector<Subsonic::RemoteAlbum> albums = Subsonic::readAlbums(parsedOk(kNewestXml));
    CHECK(albums.size() == 1);                    // the idless row is dropped
    const QVector<MusicLibrary::Album> rows = Subsonic::albumRows(A, albums);
    CHECK(rows.size() == 1);
    if (rows.size() != 1) return;
    CHECK(rows.at(0).key == Subsonic::qualify(A, Subsonic::Kind::Album, QStringLiteral("al-20")));
    CHECK(rows.at(0).year == 2024);
    CHECK(rows.at(0).trackCount == 9);            // the server's count; the songs arrive on drill
    CHECK(rows.at(0).tracks.isEmpty());
    // ...and a real ALBUM id, so opening it is the ordinary album level and the ordinary getAlbum.
    CHECK(Subsonic::parse(rows.at(0).key).kind == Subsonic::Kind::Album);

    CHECK(Subsonic::readAlbums(parsedOk(
              "<subsonic-response status=\"ok\" version=\"1.16.1\"><albumList2/></subsonic-response>"))
              .isEmpty());
}

// THE #160 LESSON, ONE LEVEL DOWN. Increment 5 proved it for artists, albums and tracks; every id this
// increment mints has to hold the same property, because a bare playlist id from two servers is exactly the
// corruption the issue opened by warning about.
static void testTwoServersCollidingSectionIds()
{
    const QString A = mkServerId();
    const QString B = mkServerId();

    const QVector<Subsonic::RemotePlaylist> pls = Subsonic::readPlaylists(parsedOk(kPlaylistsXml));
    const QVector<MusicLibrary::Album> rowsA = Subsonic::playlistRows(A, pls);
    const QVector<MusicLibrary::Album> rowsB = Subsonic::playlistRows(B, pls);
    CHECK(rowsA.size() == rowsB.size());
    for (int i = 0; i < rowsA.size() && i < rowsB.size(); ++i)
        CHECK(rowsA[i].key != rowsB[i].key);       // the SAME remote id, two different keys

    // ...and an id from A does not resolve against B's index, which is the property that matters: the
    // lookup that would have played somebody else's record cannot even be spelled.
    MusicLibrary::Index sectionsA, sectionsB;
    const Subsonic::Node root = parsedOk(kPlaylistXml);
    const QVector<Subsonic::RemotePlaylist> one = Subsonic::readPlaylists(root);
    CHECK(!one.isEmpty());
    if (one.isEmpty()) return;
    Subsonic::adoptPlaylist(sectionsA, A, one.first(), Subsonic::readSongs(root));
    Subsonic::adoptPlaylist(sectionsB, B, one.first(), Subsonic::readSongs(root));
    const QString keyA = Subsonic::qualify(A, Subsonic::Kind::Playlist, one.first().id);
    const QString keyB = Subsonic::qualify(B, Subsonic::Kind::Playlist, one.first().id);
    CHECK(keyA != keyB);
    CHECK(sectionsA.album(keyA) != nullptr);
    CHECK(sectionsA.album(keyB) == nullptr);       // B's playlist is NOT in A's index
    CHECK(sectionsB.album(keyA) == nullptr);

    // The invented containers carry their server too, so two servers' starred records are two records.
    CHECK(Subsonic::starredTracksKey(A) != Subsonic::starredTracksKey(B));
    CHECK(Subsonic::parse(Subsonic::starredTracksKey(A)).serverId == A);
}

// THE UNION RULE. Enforced by the function's SHAPE - there is no removal to get wrong - and asserted here
// because "the starred read emptied my shelf" is a bug found when somebody's favourites are already gone.
static void testStarredUnionNeverReplaces()
{
    const QString A = mkServerId();
    const QVector<Subsonic::RemoteSong> songs = Subsonic::readSongs(parsedOk(kStarredXml));
    CHECK(songs.size() == 2);
    if (songs.size() != 2) return;

    // Nothing favourite yet: both are additions, in the server's order.
    QSet<QString> have;
    QVector<Subsonic::StarredFavorite> add = Subsonic::starredAdditions(A, songs, have);
    CHECK(add.size() == 2);
    if (add.size() != 2) return;
    CHECK(add.at(0).itemId == Subsonic::qualify(A, Subsonic::Kind::Track, QStringLiteral("s-11")));
    // The id a track ROW carries, or the heart on the row cannot find the favourite it just wrote.
    CHECK(add.at(0).title == QStringLiteral("Loved track"));
    CHECK(add.at(0).subtitle == QStringLiteral("Foxglove"));

    // One already starred locally: it is not offered again, and - the point - the OTHER local favourites
    // are not mentioned at all, because there is no way for this function to say "remove".
    have.insert(Subsonic::qualify(A, Subsonic::Kind::Track, QStringLiteral("s-11")));
    have.insert(QStringLiteral("tt0111161"));                        // a film
    have.insert(QStringLiteral("C:/Games/Sonic.md"));                // a game
    have.insert(Subsonic::qualify(mkServerId(), Subsonic::Kind::Track, QStringLiteral("s-11")));
    const int before = int(have.size());
    add = Subsonic::starredAdditions(A, songs, have);
    CHECK(add.size() == 1);
    CHECK(int(have.size()) == before);            // the input set is untouched: nothing was removed
    if (add.size() == 1)
        CHECK(add.at(0).itemId == Subsonic::qualify(A, Subsonic::Kind::Track, QStringLiteral("s-12")));

    // An EMPTY starred list adds nothing - and, again, removes nothing, because it cannot.
    CHECK(Subsonic::starredAdditions(A, {}, have).isEmpty());

    // A duplicate in the server's own answer is added once.
    QVector<Subsonic::RemoteSong> dupes; dupes << songs.at(1) << songs.at(1);
    CHECK(Subsonic::starredAdditions(A, dupes, {}).size() == 1);

    // A song with no id mints no favourite: one filed under nothing can never be found again and can never
    // be un-starred.
    Subsonic::RemoteSong idless; idless.title = QStringLiteral("Nameless");
    CHECK(Subsonic::starredAdditions(A, { idless }, {}).isEmpty());
}

// ==================================================================================================
// 10. TELLING THE SERVER: the scrobble and star parameters, and how an answer ends
// ==================================================================================================
static QString paramValue(const QList<QPair<QString, QString>>& ps, const QString& k)
{
    for (const auto& p : ps) if (p.first == k) return p.second;
    return QString();
}

static void testScrobbleAndStarParams()
{
    // --- MILLISECONDS. The spec's unit, and the easiest thing in a Subsonic client to get wrong: seconds
    //     sent as milliseconds land the play in January 1970, where nothing shows it and nothing complains.
    const qint64 t1 = 1700000000LL, t2 = 1700000300LL;
    QList<QPair<QString, QString>> ps =
        Subsonic::scrobbleParams({ QStringLiteral("s-1"), QStringLiteral("s-2") }, { t1, t2 }, true);
    CHECK(paramValue(ps, QStringLiteral("submission")) == QStringLiteral("true"));
    int ids = 0, times = 0;
    for (const auto& p : ps)
    {
        if (p.first == QStringLiteral("id")) ++ids;
        if (p.first == QStringLiteral("time")) ++times;
    }
    CHECK(ids == 2);
    CHECK(times == 2);                       // repeated pairs: one request for a queue's worth
    CHECK(ps.size() == 5);
    if (ps.size() == 5)
    {
        CHECK(ps.at(0).first == QStringLiteral("id") && ps.at(0).second == QStringLiteral("s-1"));
        CHECK(ps.at(1).first == QStringLiteral("time"));
        CHECK(ps.at(1).second == QString::number(t1 * 1000LL));
        CHECK(ps.at(3).second == QString::number(t2 * 1000LL));
    }

    // --- the EPHEMERAL form: submission=false, and no time at all. A now-playing hint is about this moment
    //     by definition; a time on it would be a stale claim the instant it arrived.
    ps = Subsonic::scrobbleParams({ QStringLiteral("s-1") }, {}, false);
    CHECK(paramValue(ps, QStringLiteral("submission")) == QStringLiteral("false"));
    CHECK(paramValue(ps, QStringLiteral("id")) == QStringLiteral("s-1"));
    CHECK(paramValue(ps, QStringLiteral("time")).isEmpty());

    // A non-positive time is OMITTED rather than sent as 0: "no time" means "now" everywhere, and 0 means
    // 1970 everywhere.
    ps = Subsonic::scrobbleParams({ QStringLiteral("s-1") }, { qint64(0) }, true);
    CHECK(paramValue(ps, QStringLiteral("time")).isEmpty());

    // NOTHING TO SAY produces no parameters at all, so the caller cannot make an empty request.
    CHECK(Subsonic::scrobbleParams({}, {}, true).isEmpty());
    CHECK(Subsonic::scrobbleParams({ QString() }, {}, true).isEmpty());

    // --- star: THREE NAMESPACES, three parameter names. Sending an album's id as `id` stars whichever SONG
    //     happens to carry that id - a star landing on something the user never pressed.
    CHECK(Subsonic::starParams(Subsonic::Kind::Track, QStringLiteral("s-1")).size() == 1);
    CHECK(paramValue(Subsonic::starParams(Subsonic::Kind::Track, QStringLiteral("s-1")),
                     QStringLiteral("id")) == QStringLiteral("s-1"));
    CHECK(paramValue(Subsonic::starParams(Subsonic::Kind::Album, QStringLiteral("al-1")),
                     QStringLiteral("albumId")) == QStringLiteral("al-1"));
    CHECK(paramValue(Subsonic::starParams(Subsonic::Kind::Artist, QStringLiteral("ar-1")),
                     QStringLiteral("artistId")) == QStringLiteral("ar-1"));
    // ...and the kinds that cannot be starred produce NOTHING, so no request is made. The Virtual one is
    // the structural claim: a container this app invented is never put in a request, which is what makes
    // its id incapable of colliding with anything the server minted.
    CHECK(Subsonic::starParams(Subsonic::Kind::Cover, QStringLiteral("c-1")).isEmpty());
    CHECK(Subsonic::starParams(Subsonic::Kind::Virtual, QStringLiteral("starred")).isEmpty());
    CHECK(Subsonic::starParams(Subsonic::Kind::Playlist, QStringLiteral("pl-1")).isEmpty());
    CHECK(Subsonic::starParams(Subsonic::Kind::Track, QString()).isEmpty());
}

// ---- RECENTLY ADDED IS PAGED (issue #298) -----------------------------------------------------------
// getAlbumList2 answers with a WINDOW and says nothing about what is behind it. One window rendered as the
// whole level truncated a library with more than 100 albums in it, silently - and a silent truncation is
// indistinguishable from a complete answer, which is the one thing this client is otherwise careful about.
static void testNewestPaging()
{
    // THE PARAMETER THAT WAS MISSING. Without `offset` every request is the first window and the level can
    // never grow, however far the user scrolls.
    const auto first = Subsonic::albumListParams(QStringLiteral("newest"), 100, 0);
    CHECK(paramValue(first, QStringLiteral("type")) == QStringLiteral("newest"));
    CHECK(paramValue(first, QStringLiteral("size")) == QStringLiteral("100"));
    CHECK(paramValue(first, QStringLiteral("offset")) == QStringLiteral("0"));
    const auto second = Subsonic::albumListParams(QStringLiteral("newest"), 100, 100);
    CHECK(paramValue(second, QStringLiteral("offset")) == QStringLiteral("100"));
    // ...sent even at zero, so a server that reads it and a server that ignores it are the same server, and
    // there is one request shape rather than two.
    CHECK(first.size() == second.size());
    // A negative offset is a bug upstream, not a request: it is clamped rather than sent.
    CHECK(paramValue(Subsonic::albumListParams(QStringLiteral("newest"), 100, -5),
                     QStringLiteral("offset")) == QStringLiteral("0"));
    // NOTHING TO ASK FOR produces no parameters at all, so the caller cannot make a meaningless request.
    CHECK(Subsonic::albumListParams(QString(), 100, 0).isEmpty());
    CHECK(Subsonic::albumListParams(QStringLiteral("newest"), 0, 0).isEmpty());

    // IS THERE MORE? A FULL window means there may be; a short one is the end of the list, which is all this
    // protocol ever says about it.
    CHECK(Subsonic::morePagesLikely(/*returned*/ 100, /*window*/ 100, /*addedNew*/ 100));
    CHECK(!Subsonic::morePagesLikely(99, 100, 99));
    CHECK(!Subsonic::morePagesLikely(0, 100, 0));
    // ...AND THE DEFENSIVE ARM. A server that ignores `offset` answers the second window with the first one:
    // a full window that added nothing new. Paging on would grow the same albums for ever while the user
    // scrolled, so the list ends here instead.
    CHECK(!Subsonic::morePagesLikely(100, 100, 0));
    CHECK(Subsonic::morePagesLikely(100, 100, 1));
    CHECK(!Subsonic::morePagesLikely(100, 0, 100));

    // AND THE ROWS OF A LATER WINDOW ARE ORDINARY ALBUM ROWS. A page is not a different kind of thing: the
    // level appends them with the builder it drew the first window with, so an album that arrived fourth
    // opens exactly as one that arrived first.
    const QString A = mkServerId();
    const QVector<MusicLibrary::Album> rows = Subsonic::albumRows(A, Subsonic::readAlbums(parsedOk(kNewestXml)));
    CHECK(rows.size() == 1);
    if (rows.isEmpty()) return;
    CHECK(!rows.at(0).key.isEmpty());
    CHECK(Subsonic::parse(rows.at(0).key).kind == Subsonic::Kind::Album);
    // The duplicate guard the client pages with is the album KEY, which is server-qualified - so the same
    // remote id from two servers is two rows and never one deduped away.
    const QString B = mkServerId();
    const QVector<MusicLibrary::Album> other = Subsonic::albumRows(B, Subsonic::readAlbums(parsedOk(kNewestXml)));
    CHECK(!other.isEmpty() && other.at(0).key != rows.at(0).key);
}

static void testFate()
{
    using F = Subsonic::Fate;
    auto fateOfBody = [](const char* body) {
        bool ok = false;
        return Subsonic::fateOf(Subsonic::envelopeOf(Subsonic::parseBody(QByteArray(body), &ok)));
    };
    CHECK(fateOfBody("<subsonic-response status=\"ok\" version=\"1.16.1\"/>") == F::Ok);
    // A REFUSED CREDENTIAL: keep the listens (the user can fix the token and they still land) but stop
    // pumping - retrying a bad token in a loop is how an account gets rate-limited.
    CHECK(fateOfBody(kFailXml) == F::Auth);
    // "Not found": the track was deleted or the library rescanned. No retry brings it back, and keeping it
    // jams every listen behind it for ever.
    CHECK(fateOfBody("<subsonic-response status=\"failed\"><error code=\"70\" message=\"Gone.\"/>"
                     "</subsonic-response>") == F::Rejected);
    CHECK(fateOfBody("<subsonic-response status=\"failed\"><error code=\"10\" message=\"Missing.\"/>"
                     "</subsonic-response>") == F::Rejected);
    // A server error is worth trying again.
    CHECK(fateOfBody("<subsonic-response status=\"failed\"><error code=\"0\" message=\"Oops.\"/>"
                     "</subsonic-response>") == F::Retryable);
    CHECK(fateOfBody("<subsonic-response status=\"failed\"><error code=\"50\" message=\"No.\"/>"
                     "</subsonic-response>") == F::Retryable);
    // NOT A SUBSONIC RESPONSE AT ALL - a proxy's error page, a captive portal. Retryable: every one of
    // those goes away on its own, and dropping the listens over it loses them to a router reboot.
    CHECK(fateOfBody("<html><body>502 Bad Gateway</body></html>") == F::Retryable);
    CHECK(fateOfBody("") == F::Retryable);
}

// ==================================================================================================
// 11. The three levels, rendered by the builders #74 already had
// ==================================================================================================
static void testSectionLevels()
{
    const QString A = mkServerId();
    const Subsonic::Starred s = Subsonic::readStarred(A, parsedOk(kStarredXml));
    const browse::MusicEmptyNote note;
    auto noCover = [](const MusicLibrary::Album&) { return QString(); };
    const MediaCatalog cat = browse::musicSectionCatalog(QStringLiteral("Starred"), s.artists, s.albums,
                                                         s.tracks, note, noCover);
    CHECK(cat.title == QStringLiteral("Starred"));
    CHECK(cat.items.size() == 4);              // 1 artist + 1 album + 2 tracks
    if (cat.items.size() != 4) return;
    // THE SAME ROW TYPES every other music level uses - that is the whole "same UI, different supplier"
    // claim, and a new type here would be a second browse tree by another name.
    CHECK(cat.items.at(0).type == QString::fromLatin1(browse::kMusicArtistType));
    CHECK(cat.items.at(1).type == QString::fromLatin1(browse::kMusicAlbumType));
    CHECK(cat.items.at(2).type == QString::fromLatin1(browse::kMusicTrackType));
    CHECK(cat.items.at(0).expandable);
    CHECK(cat.items.at(1).expandable);
    // ...routing through the SAME prefixes, so a starred album opens the ordinary album level.
    CHECK(cat.items.at(1).mime == QString::fromLatin1(browse::kMusicAlbumPrefix) + s.albums.at(0).key);
    // ...and a loose track row is queued behind the starred record.
    CHECK(cat.items.at(2).mime
          == QString::fromLatin1(browse::kMusicTrackPrefix) + Subsonic::starredTracksKey(A));
    CHECK(cat.items.at(2).id == s.tracks.at(0).path);      // the id a favourite is filed under
    // NO "Play all" / "Shuffle all" here: the tracks behind an album row have not been fetched, so such a
    // verb could only produce an empty queue, and offering one that can only no-op is worse than not.
    for (const MediaItem& it : cat.items)
    {
        CHECK(it.type != QString::fromLatin1(browse::kMusicPlayArtistType));
        CHECK(it.type != QString::fromLatin1(browse::kMusicShuffleArtistType));
        CHECK(it.type != QString::fromLatin1(browse::kMusicShuffleAllType));
    }

    // A PLAYLIST LEVEL is album rows, and every one of them routes to the ordinary album level.
    const QVector<MusicLibrary::Album> pls =
        Subsonic::playlistRows(A, Subsonic::readPlaylists(parsedOk(kPlaylistsXml)));
    const MediaCatalog plc = browse::musicSectionCatalog(QStringLiteral("Playlists"), {}, pls, {}, note,
                                                         noCover);
    CHECK(plc.items.size() == 2);
    for (const MediaItem& it : plc.items)
    {
        CHECK(it.type == QString::fromLatin1(browse::kMusicAlbumType));
        CHECK(it.expandable);
    }

    // AN EMPTY SECTION IS EXPLAINED rather than left blank - the rule the Music root already follows.
    browse::MusicEmptyNote said;
    said.text = QStringLiteral("You have not starred anything on this music server yet.");
    const MediaCatalog empty = browse::musicSectionCatalog(QStringLiteral("Starred"), {}, {}, {}, said,
                                                           noCover);
    CHECK(empty.items.size() == 1);
    if (empty.items.size() == 1)
    {
        CHECK(empty.items.at(0).type == QStringLiteral("info"));
        CHECK(empty.items.at(0).title == said.text);
    }
    // ...and with no note there is nothing fabricated: an empty catalog stays empty.
    CHECK(browse::musicSectionCatalog(QStringLiteral("Starred"), {}, {}, {},
                                      browse::MusicEmptyNote{}, noCover).items.isEmpty());
}

// THE COMPATIBILITY CLAIM. The three doors appear INSIDE a server and nowhere else, so the Music root of an
// install with a local library is byte-for-byte the catalog it was before this increment existed.
static void testDoorsOnlyInsideAServer(const MusicLibrary::Index& local)
{
    const QString A = mkServerId();
    auto noCover = [](const MusicLibrary::Album&) { return QString(); };

    const MediaCatalog root = browse::musicArtistsCatalog(local, browse::MusicEmptyNote{}, noCover, 1);
    for (const MediaItem& it : root.items)
    {
        CHECK(it.type != QString::fromLatin1(browse::kMusicPlaylistsType));
        CHECK(it.type != QString::fromLatin1(browse::kMusicStarredType));
        CHECK(it.type != QString::fromLatin1(browse::kMusicNewestType));
    }

    const MediaCatalog inside =
        browse::musicArtistsCatalog(local, browse::MusicEmptyNote{}, noCover, 0, A);
    int doors = 0;
    for (const MediaItem& it : inside.items)
    {
        if (it.type == QString::fromLatin1(browse::kMusicPlaylistsType)
            || it.type == QString::fromLatin1(browse::kMusicStarredType)
            || it.type == QString::fromLatin1(browse::kMusicNewestType))
        {
            ++doors;
            // KEYED BY THE SERVER, so a door pressed on one server's level cannot open another's.
            CHECK(it.mime.endsWith(A));
            CHECK(it.expandable);
        }
    }
    CHECK(doors == 3);

    // ...and an EMPTY server still shows them: a server whose artist list has not arrived may still have
    // playlists and starred tracks, and a bare "nothing here" over the top of them is simply wrong.
    browse::MusicEmptyNote said; said.text = QStringLiteral("Nothing yet.");
    const MediaCatalog bare = browse::musicArtistsCatalog(MusicLibrary::Index{}, said, noCover, 0, A);
    int bareDoors = 0;
    for (const MediaItem& it : bare.items)
        if (it.type == QString::fromLatin1(browse::kMusicStarredType)
            || it.type == QString::fromLatin1(browse::kMusicPlaylistsType)
            || it.type == QString::fromLatin1(browse::kMusicNewestType)) ++bareDoors;
    CHECK(bareDoors == 3);
}

// ==================================================================================================
// 12. THE CREDENTIAL BYTE-SCAN
// ==================================================================================================
// An ASSERTION, not a claim in a report. Everything this increment WRITES anywhere - a browse row, a
// persisted playlist record, a favourite's id, the request parameters themselves - is scanned for the
// password, the token and the salt. The scan is over BYTES rather than a formatted string, because what is
// being defended against is a value ending up somewhere by accident rather than by being printed.
static void testNoCredentialAnywhere()
{
    const QString A = mkServerId();
    const QString salt = Subsonic::saltFrom(Q_UINT64_C(0x0123456789abcdef));
    const QString token = Subsonic::tokenFor(QString::fromLatin1(kPassword), salt);
    CHECK(!token.isEmpty() && !salt.isEmpty());

    QByteArray scanned;
    auto eat = [&scanned](const QString& s) { scanned += s.toUtf8(); scanned += '\n'; };

    // Everything the three levels produce.
    const Subsonic::Node pr = parsedOk(kPlaylistXml);
    const QVector<Subsonic::RemotePlaylist> pl = Subsonic::readPlaylists(pr);
    MusicLibrary::Index sections;
    if (!pl.isEmpty()) Subsonic::adoptPlaylist(sections, A, pl.first(), Subsonic::readSongs(pr));
    const Subsonic::Starred st = Subsonic::readStarred(A, parsedOk(kStarredXml));
    Subsonic::adoptStarred(sections, A, st);
    for (const MusicLibrary::Artist& a : sections.artists)
    {
        eat(a.key); eat(a.name);
        for (const MusicLibrary::Album& b : a.albums)
        {
            eat(b.key); eat(b.title); eat(b.albumArtist);
            for (const MusicLibrary::IndexTrack& t : b.tracks)
                { eat(t.path); eat(t.sourcePath); eat(t.title); eat(t.albumKey); }
        }
    }
    // ...the rows a level draws, in full.
    const MediaCatalog cat = browse::musicSectionCatalog(QStringLiteral("Starred"), st.artists, st.albums,
                                                         st.tracks, browse::MusicEmptyNote{},
                                                         [](const MusicLibrary::Album&) { return QString(); });
    for (const MediaItem& it : cat.items)
        { eat(it.id); eat(it.mime); eat(it.url); eat(it.title); eat(it.subtitle); eat(it.thumbnailUrl); }
    // ...and the favourites a starred read would write.
    for (const Subsonic::StarredFavorite& f :
             Subsonic::starredAdditions(A, Subsonic::readSongs(parsedOk(kStarredXml)), {}))
        { eat(f.itemId); eat(f.title); eat(f.subtitle); }
    // ...and the scrobble/star parameters, which are the only place an id is allowed to travel at all.
    for (const auto& kv : Subsonic::scrobbleParams({ QStringLiteral("s-9") }, { qint64(1700000000) }, true))
        { eat(kv.first); eat(kv.second); }
    for (const auto& kv : Subsonic::starParams(Subsonic::Kind::Track, QStringLiteral("s-9")))
        { eat(kv.first); eat(kv.second); }

    CHECK(!scanned.contains(QByteArray(kPassword)));
    CHECK(!scanned.contains(token.toUtf8()));
    CHECK(!scanned.contains(salt.toUtf8()));
    // ...and nothing that even LOOKS like a signed request: the whole family of "somebody stored the url".
    CHECK(!scanned.contains(QByteArray("&t=")));
    CHECK(!scanned.contains(QByteArray("&s=")));
    CHECK(!scanned.contains(QByteArray("/rest/")));
}

// ==================================================================================================
// #370 — AN EMPTY COVER ANSWER MUST NOT LOOP
// ==================================================================================================
// The one part of this file that opens a socket, and it only ever opens one to itself: a loopback stub
// this probe stands up, speaking just enough HTTP/1.1 to answer the requests below. No real server, no
// account, and the password is still the literal above.
//
// THE BUG. prefetchAlbumCover's `then` means "new artwork landed, re-render", and the level wires it to a
// debounced loadTop that re-runs the prefetch over the same rows (HomeView::scheduleMusicArtRefresh). An
// answer that stored nothing - an empty body, which MetaCache::storeImage silently drops - fired `then`
// anyway, so the re-render found nothing on disk and nothing in flight, and asked again. For ever.
static const char* kUser    = "probe-user";
static const char* kJfToken = "probe-fixture-jellyfin-token-3b7e";   // distinctive: a byte scan must not match by accident
static const char* kJfServerId = "0123456789abcdef0123456789abcdef";  // Jellyfin ids are 32 hex digits
static const char* kShelfId = "probe-shelf";

class CoverStub : public QTcpServer
{
public:
    enum class Answer { Empty, Image, ServerError, NotFoundEnvelope, AuthEnvelope, Http404 };
    QHash<QString, Answer> answers;   // cover id -> what asking for it gets. Unlisted ids answer Empty.
    QByteArray imageBytes;
    QString    root;                  // http://127.0.0.1:<port>, once listening
    QStringList coverIds;             // every cover request, by the id it asked for - in order
    // Every request target. THESE HOLD THE SUBSONIC TOKEN AND SALT and are never printed; they exist so
    // the byte scan at the end can harvest exactly what went over the wire.
    QStringList targets;

    explicit CoverStub(QObject* parent = nullptr) : QTcpServer(parent) {}
    int covers(const QString& id) const { return int(coverIds.count(id)); }

protected:
    void incomingConnection(qintptr handle) override
    {
        auto* sock = new QTcpSocket(this);
        sock->setSocketDescriptor(handle);
        connect(sock, &QTcpSocket::readyRead, this, [this, sock] {
            sock->setProperty("buf", sock->property("buf").toByteArray() + sock->readAll());
            const QByteArray buf = sock->property("buf").toByteArray();
            if (buf.indexOf("\r\n\r\n") < 0) return;   // GETs only: the head is the whole request
            if (sock->property("done").toBool()) return;
            sock->setProperty("done", true);
            const QList<QByteArray> reqLine = buf.left(buf.indexOf("\r\n")).split(' ');
            reply(sock, QString::fromUtf8(reqLine.value(1)));
        });
    }

private:
    static void send(QTcpSocket* sock, int status, const QByteArray& type, const QByteArray& body)
    {
        sock->write("HTTP/1.1 " + QByteArray::number(status) + (status == 200 ? " OK" : " ERR")
                    + "\r\nContent-Type: " + type
                    + "\r\nContent-Length: " + QByteArray::number(body.size())
                    + "\r\nConnection: close\r\n\r\n" + body);
        sock->flush();
        sock->disconnectFromHost();
    }

    void answerCover(QTcpSocket* sock, const QString& id)
    {
        coverIds.push_back(id);
        switch (answers.value(id, Answer::Empty))
        {
            case Answer::Empty:       send(sock, 200, "image/jpeg", QByteArray()); return;
            case Answer::Image:       send(sock, 200, "image/png", imageBytes); return;
            case Answer::ServerError: send(sock, 500, "text/plain", "busy"); return;
            case Answer::Http404:     send(sock, 404, "text/plain", "no such image"); return;
            // THE PROTOCOL'S OWN "NO SUCH COVER": a 200, with a failure envelope inside. Code 70 is "the
            // requested data was not found"; 40 is a refused credential.
            case Answer::NotFoundEnvelope:
                send(sock, 200, "text/xml", "<subsonic-response status=\"failed\" version=\"1.16.1\">"
                                            "<error code=\"70\" message=\"Cover art not found\"/>"
                                            "</subsonic-response>");
                return;
            case Answer::AuthEnvelope:
                send(sock, 200, "text/xml", "<subsonic-response status=\"failed\" version=\"1.16.1\">"
                                            "<error code=\"40\" message=\"Wrong username or password\"/>"
                                            "</subsonic-response>");
                return;
        }
    }

    void reply(QTcpSocket* sock, const QString& target)
    {
        targets.push_back(target);
        const QUrl u(target);
        const QString path = u.path();
        if (path == QLatin1String("/rest/getStarred2.view"))
        {
            // Every starred record has a DIFFERENT cover id from its album id, so a client that asked for
            // the album id rather than the cover id would be asking the stub for something it never lists.
            QByteArray body = "<subsonic-response status=\"ok\" version=\"1.16.1\"><starred2>";
            for (int i = 1; i <= 6; ++i)
                body += "<album id=\"al-" + QByteArray::number(i) + "\" name=\"Record " + QByteArray::number(i)
                      + "\" artist=\"Probe\" artistId=\"ar-1\" songCount=\"1\" coverArt=\"c-"
                      + QByteArray::number(i) + "\"/>";
            body += "</starred2></subsonic-response>";
            send(sock, 200, "text/xml", body);
            return;
        }
        if (path == QLatin1String("/rest/getCoverArt.view"))
        {
            answerCover(sock, QUrlQuery(u).queryItemValue(QStringLiteral("id")));
            return;
        }
        // Jellyfin: /Items/<id>/Images/Primary
        if (path.startsWith(QLatin1String("/Items/")) && path.endsWith(QLatin1String("/Images/Primary")))
        {
            answerCover(sock, path.section(QLatin1Char('/'), 2, 2));
            return;
        }
        // The EverythingBox server's music shelf: one artist's albums, then each album's own image url.
        if (path.startsWith(QLatin1String("/detail/")))
        {
            const QByteArray r = root.toUtf8();
            send(sock, 200, "application/json",
                 "{\"items\":["
                 "{\"id\":\"sm-1\",\"title\":\"Empty sleeve\",\"type\":\"album\",\"thumbnailUrl\":\"" + r + "/cover/sm-1\"},"
                 "{\"id\":\"sm-2\",\"title\":\"No sleeve at all\",\"type\":\"album\"},"
                 "{\"id\":\"sm-3\",\"title\":\"Real sleeve\",\"type\":\"album\",\"thumbnailUrl\":\"" + r + "/cover/sm-3\"},"
                 "{\"id\":\"sm-4\",\"title\":\"Busy sleeve\",\"type\":\"album\",\"thumbnailUrl\":\"" + r + "/cover/sm-4\"}"
                 "]}");
            return;
        }
        if (path.startsWith(QLatin1String("/cover/")))
        {
            answerCover(sock, path.mid(7));
            return;
        }
        send(sock, 404, "text/plain", "");
    }
};

static bool waitFor(const std::function<bool()>& done, int ms = 5000)
{
    QDeadlineTimer dl(ms);
    while (!done() && !dl.hasExpired())
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return done();
}
static void settleFor(int ms) { waitFor([] { return false; }, ms); }

// A LEVEL, REDUCED TO WHAT MATTERS HERE. It draws, it asks for the art of what it drew, and a landing arms
// ONE re-render - which is HomeView::scheduleMusicArtRefresh exactly: a debounced loadTop that re-runs the
// prefetch over the same rows. kCap only bounds the broken loop so a red run terminates; a correct client
// never gets anywhere near it.
class ArtView
{
public:
    using Prefetch = std::function<void(const QString&, std::function<void()>)>;
    static constexpr int kCap = 25;
    ArtView(Prefetch p, QString key) : prefetch_(std::move(p)), key_(std::move(key)) {}
    void render()
    {
        ++renders;
        prefetch_(key_, [this] { ++landed; schedule(); });
    }
    int renders = 0;
    int landed  = 0;    // how many times the client said "new artwork landed"
private:
    void schedule()
    {
        if (pending_ || renders >= kCap) return;
        pending_ = true;
        QTimer::singleShot(0, &ctx_, [this] { pending_ = false; render(); });
    }
    Prefetch prefetch_;
    QString  key_;
    bool     pending_ = false;
    QObject  ctx_;
};

// Views live until the process ends: a reply that lands after its test returned still calls back into its
// view, and the broken client in a red run does exactly that.
static ArtView& newView(ArtView::Prefetch p, const QString& key)
{
    static std::vector<std::unique_ptr<ArtView>> views;
    views.push_back(std::make_unique<ArtView>(std::move(p), key));
    return *views.back();
}

// Let a view's first pass play out: either it loops to the cap (broken), or nothing more happens.
// (The bound is how long "nothing more happens" is watched for. The broken client reached the cap well inside
// it on loopback; nothing is ever asserted about time.)
static void settleView(const ArtView& v) { waitFor([&] { return v.renders >= ArtView::kCap; }, 700); }

// One more pass of the level, for a reason that has nothing to do with this cover - another row's art
// landed, the index changed, the user came back to the level. Long enough for any request it causes to be
// answered and read.
static void anotherPass(ArtView& v, const CoverStub& stub, const QString& coverId)
{
    const int before = stub.covers(coverId);
    v.render();
    waitFor([&] { return stub.covers(coverId) > before; }, 300);
    settleFor(100);
}

static QString coverOf(const QString& key) { return MetaCache::imagePath(key, QStringLiteral("cover")); }

static void testSubsonicCoverAnswers(CoverStub& stub, const QString& serverId)
{
    SubsonicClient& cl = SubsonicClient::instance();
    bool done = false, ok = false;
    cl.fetchStarred(serverId, [&](const SubsonicClient::Result& r) { done = true; ok = r.ok; });
    CHECK(waitFor([&] { return done; }));
    CHECK(ok);
    auto key = [&](int i) {
        return Subsonic::qualify(serverId, Subsonic::Kind::Album, QStringLiteral("al-%1").arg(i));
    };
    const ArtView::Prefetch prefetch = [&cl](const QString& k, std::function<void()> t) {
        cl.prefetchAlbumCover(k, std::move(t));
    };

    // ---- 1. AN EMPTY ANSWER, with the refresh wired the way the level wires it: asked ONCE ----------------
    {
        stub.answers[QStringLiteral("c-1")] = CoverStub::Answer::Empty;
        ArtView& v = newView(prefetch, key(1));
        v.render();
        CHECK(waitFor([&] { return stub.covers(QStringLiteral("c-1")) >= 1; }));
        settleView(v);
        std::printf("370 subsonic: empty answer, level wired as the app wires it -> %d getCoverArt request(s), "
                    "%d render(s)\n", stub.covers(QStringLiteral("c-1")), v.renders);
        CHECK(stub.covers(QStringLiteral("c-1")) == 1);
        CHECK(v.landed == 0);                        // nothing landed, so nothing may say it did
        CHECK(coverOf(key(1)).isEmpty());
        // ...and ONCE PER SESSION: five re-renders for other reasons ask nothing more.
        for (int i = 0; i < 5; ++i) anotherPass(v, stub, QStringLiteral("c-1"));
        std::printf("370 subsonic: ...after 5 more passes of the level -> %d request(s)\n",
                    stub.covers(QStringLiteral("c-1")));
        CHECK(stub.covers(QStringLiteral("c-1")) == 1);
        CHECK(cl.coversKnownMissing().contains(key(1)));
    }

    // ---- 2. A REAL IMAGE still lands, and says so exactly once ---------------------------------------
    {
        stub.answers[QStringLiteral("c-2")] = CoverStub::Answer::Image;
        ArtView& v = newView(prefetch, key(2));
        v.render();
        CHECK(waitFor([&] { return v.landed >= 1; }));
        settleView(v);
        CHECK(stub.covers(QStringLiteral("c-2")) == 1);
        CHECK(v.landed == 1);
        CHECK(v.renders == 2);                       // the landing re-rendered once, and that pass asked nothing
        CHECK(!coverOf(key(2)).isEmpty());
        anotherPass(v, stub, QStringLiteral("c-2"));
        CHECK(stub.covers(QStringLiteral("c-2")) == 1);
        CHECK(v.landed == 1);
    }

    // ---- 3. A FAILED REQUEST is not "no art": it may succeed next time, so a later pass asks again -----
    {
        stub.answers[QStringLiteral("c-3")] = CoverStub::Answer::ServerError;
        ArtView& v = newView(prefetch, key(3));
        v.render();
        CHECK(waitFor([&] { return stub.covers(QStringLiteral("c-3")) >= 1; }));
        settleView(v);
        std::printf("370 subsonic: HTTP 500 -> %d request(s), %d render(s)\n",
                    stub.covers(QStringLiteral("c-3")), v.renders);
        CHECK(stub.covers(QStringLiteral("c-3")) == 1);   // ...but not in a loop
        CHECK(v.landed == 0);
        CHECK(coverOf(key(3)).isEmpty());
        CHECK(!cl.coversKnownMissing().contains(key(3)));   // a failure is not "no art"
        stub.answers[QStringLiteral("c-3")] = CoverStub::Answer::Image;   // the server recovers
        anotherPass(v, stub, QStringLiteral("c-3"));
        CHECK(waitFor([&] { return v.landed >= 1; }));
        settleView(v);
        CHECK(stub.covers(QStringLiteral("c-3")) == 2);   // retried once, on the next pass, and no more
        CHECK(v.landed == 1);
        CHECK(!coverOf(key(3)).isEmpty());
    }

    // ---- 4. THE PROTOCOL'S "NOT FOUND" is an answer: remembered, and NEVER written to disk as a cover ------
    // A failure envelope is a perfectly good non-empty body. Stored as cover.jpg it would be a broken picture
    // that PERSISTS - and imagePath() would say "already on disk" in every later session, so art the server
    // gains later could never arrive.
    {
        stub.answers[QStringLiteral("c-4")] = CoverStub::Answer::NotFoundEnvelope;
        ArtView& v = newView(prefetch, key(4));
        v.render();
        CHECK(waitFor([&] { return stub.covers(QStringLiteral("c-4")) >= 1; }));
        settleView(v);
        CHECK(coverOf(key(4)).isEmpty());
        CHECK(v.landed == 0);
        for (int i = 0; i < 3; ++i) anotherPass(v, stub, QStringLiteral("c-4"));
        CHECK(stub.covers(QStringLiteral("c-4")) == 1);
        CHECK(cl.coversKnownMissing().contains(key(4)));
    }

    // ---- 5. HTTP 404 is the server saying there is no such image: an answer, remembered ----------------
    {
        stub.answers[QStringLiteral("c-5")] = CoverStub::Answer::Http404;
        ArtView& v = newView(prefetch, key(5));
        v.render();
        CHECK(waitFor([&] { return stub.covers(QStringLiteral("c-5")) >= 1; }));
        settleView(v);
        CHECK(v.landed == 0);
        for (int i = 0; i < 3; ++i) anotherPass(v, stub, QStringLiteral("c-5"));
        CHECK(stub.covers(QStringLiteral("c-5")) == 1);
    }

    // ---- 6. A refused CREDENTIAL inside the envelope is not "no art" either: not stored, not remembered --
    {
        stub.answers[QStringLiteral("c-6")] = CoverStub::Answer::AuthEnvelope;
        ArtView& v = newView(prefetch, key(6));
        v.render();
        CHECK(waitFor([&] { return stub.covers(QStringLiteral("c-6")) >= 1; }));
        settleView(v);
        CHECK(coverOf(key(6)).isEmpty());
        CHECK(v.landed == 0);
        CHECK(stub.covers(QStringLiteral("c-6")) == 1);
        anotherPass(v, stub, QStringLiteral("c-6"));
        CHECK(stub.covers(QStringLiteral("c-6")) == 2);
    }
}

// THE NEXT SESSION, for real: a second PROCESS over the same data directory. Art the server gains after an
// empty answer must reach a later session, which is the whole reason "no art" is never written down.
static int coverSessionChild(const QString& root, const QString& serverId)
{
    SubsonicServer srv;
    srv.id = serverId; srv.name = QStringLiteral("Fixture"); srv.url = root;
    srv.username = QLatin1String(kUser); srv.password = QLatin1String(kPassword); srv.allowPlainHttp = true;
    SubsonicServerStore::add(srv);   // by id: an update in place, never a second server
    bool done = false;
    SubsonicClient::instance().fetchStarred(serverId, [&](const SubsonicClient::Result&) { done = true; });
    if (!waitFor([&] { return done; })) return 2;
    const QString key = Subsonic::qualify(serverId, Subsonic::Kind::Album, QStringLiteral("al-1"));
    if (!coverOf(key).isEmpty()) return 3;      // the first session must not have left anything behind
    bool landed = false;
    SubsonicClient::instance().prefetchAlbumCover(key, [&] { landed = true; });
    if (!waitFor([&] { return landed; })) return 4;
    return coverOf(key).isEmpty() ? 5 : 0;
}

static void testNextSessionAsksAgain(CoverStub& stub, const QString& serverId)
{
    const int before = stub.covers(QStringLiteral("c-1"));
    CHECK(before == 1);                          // the first session asked once and got nothing
    stub.answers[QStringLiteral("c-1")] = CoverStub::Answer::Image;   // ...and the server has art now
    QProcess child;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("EB_PROBE_DATA_DIR"), AppPaths::dataDir());
    child.setProcessEnvironment(env);
    child.start(QCoreApplication::applicationFilePath(),
                { QStringLiteral("cover-session"), stub.root, serverId });
    CHECK(waitFor([&] { return child.state() == QProcess::NotRunning; }, 30000));
    std::printf("370 subsonic: next session -> exit %d, %d request(s) for that cover in total\n",
                child.exitCode(), stub.covers(QStringLiteral("c-1")));
    CHECK(child.exitStatus() == QProcess::NormalExit);
    CHECK(child.exitCode() == 0);
    CHECK(stub.covers(QStringLiteral("c-1")) == before + 1);
}

// THE SAME SHAPE, TWICE MORE. The Jellyfin and EverythingBox-server music clients fed the same level through
// the same callback - and fired it on EVERY path: for a cover already on disk, for an album with no art,
// for a failure. With the callback wired to a re-render that re-runs the prefetch, each of those is a level
// that reloads itself every 400 ms for as long as it is on screen.
static void testJellyfinCoverAnswers(CoverStub& stub)
{
    JellyfinServer jf;
    jf.id = QLatin1String(kJfServerId); jf.name = QStringLiteral("Fixture JF"); jf.url = stub.root;
    jf.userId = QStringLiteral("u1"); jf.userName = QStringLiteral("probe");
    jf.token = QLatin1String(kJfToken); jf.allowPlainHttp = true; jf.enabled = true;
    CHECK(JellyfinServerStore::add(jf));
    JellyfinMusicClient& cl = JellyfinMusicClient::instance();
    const ArtView::Prefetch prefetch = [&cl](const QString& k, std::function<void()> t) {
        cl.prefetchAlbumCover(k, std::move(t));
    };
    auto key = [](const char* item) { return Jellyfin::qualify(QLatin1String(kJfServerId), QLatin1String(item)); };

    // A COVER ALREADY ON DISK: nothing landed, nothing to re-render, nothing asked.
    MetaCache::storeImage(key("jf-1"), QStringLiteral("cover"), QStringLiteral("cover.jpg"),
                          QStringLiteral("image/png"), stub.imageBytes);
    CHECK(!coverOf(key("jf-1")).isEmpty());
    {
        ArtView& v = newView(prefetch, key("jf-1"));
        v.render();
        settleView(v);
        std::printf("370 jellyfin: cover already cached -> %d render(s)\n", v.renders);
        CHECK(v.landed == 0);
        CHECK(v.renders == 1);
        CHECK(stub.covers(QStringLiteral("jf-1")) == 0);
    }
    // AN EMPTY ANSWER: once.
    {
        ArtView& v = newView(prefetch, key("jf-2"));
        v.render();
        CHECK(waitFor([&] { return stub.covers(QStringLiteral("jf-2")) >= 1; }));
        settleView(v);
        std::printf("370 jellyfin: empty answer -> %d request(s), %d render(s)\n",
                    stub.covers(QStringLiteral("jf-2")), v.renders);
        CHECK(stub.covers(QStringLiteral("jf-2")) == 1);
        CHECK(v.landed == 0);
        for (int i = 0; i < 3; ++i) anotherPass(v, stub, QStringLiteral("jf-2"));
        CHECK(stub.covers(QStringLiteral("jf-2")) == 1);
    }
    // A REAL IMAGE: lands once.
    {
        stub.answers[QStringLiteral("jf-3")] = CoverStub::Answer::Image;
        ArtView& v = newView(prefetch, key("jf-3"));
        v.render();
        CHECK(waitFor([&] { return v.landed >= 1; }));
        settleView(v);
        CHECK(stub.covers(QStringLiteral("jf-3")) == 1);
        CHECK(v.landed == 1);
        CHECK(!coverOf(key("jf-3")).isEmpty());
    }
    // A FAILURE: not in a loop, and not remembered.
    {
        stub.answers[QStringLiteral("jf-4")] = CoverStub::Answer::ServerError;
        ArtView& v = newView(prefetch, key("jf-4"));
        v.render();
        CHECK(waitFor([&] { return stub.covers(QStringLiteral("jf-4")) >= 1; }));
        settleView(v);
        CHECK(stub.covers(QStringLiteral("jf-4")) == 1);
        CHECK(v.landed == 0);
        anotherPass(v, stub, QStringLiteral("jf-4"));
        CHECK(stub.covers(QStringLiteral("jf-4")) == 2);
    }
}

static void testServerMusicCoverAnswers(CoverStub& stub)
{
    ServerMusicClient& cl = ServerMusicClient::instance();
    ServerMusicClient::Shelf shelf;
    shelf.id = QLatin1String(kShelfId); shelf.name = QStringLiteral("Fixture shelf");
    shelf.baseUrl = stub.root; shelf.catalogId = QStringLiteral("music");
    cl.setShelves({ shelf });
    bool done = false, ok = false;
    cl.fetchArtistAlbums(ServerMusic::qualify(QLatin1String(kShelfId), ServerMusic::Kind::Artist,
                                              QStringLiteral("ar-1")),
                         [&](const ServerMusicClient::Result& r) { done = true; ok = r.ok; });
    CHECK(waitFor([&] { return done; }));
    CHECK(ok);
    const ArtView::Prefetch prefetch = [&cl](const QString& k, std::function<void()> t) {
        cl.prefetchAlbumCover(k, std::move(t));
    };
    auto key = [](const char* id) {
        return ServerMusic::qualify(QLatin1String(kShelfId), ServerMusic::Kind::Album, QLatin1String(id));
    };

    // AN ALBUM WITH NO ART AT ALL: nothing to ask for, nothing landed, nothing to re-render.
    {
        ArtView& v = newView(prefetch, key("sm-2"));
        v.render();
        settleView(v);
        std::printf("370 server shelf: album with no image url -> %d render(s)\n", v.renders);
        CHECK(v.landed == 0);
        CHECK(v.renders == 1);
    }
    // AN EMPTY ANSWER: once.
    {
        ArtView& v = newView(prefetch, key("sm-1"));
        v.render();
        CHECK(waitFor([&] { return stub.covers(QStringLiteral("sm-1")) >= 1; }));
        settleView(v);
        std::printf("370 server shelf: empty answer -> %d request(s), %d render(s)\n",
                    stub.covers(QStringLiteral("sm-1")), v.renders);
        CHECK(stub.covers(QStringLiteral("sm-1")) == 1);
        CHECK(v.landed == 0);
        for (int i = 0; i < 3; ++i) anotherPass(v, stub, QStringLiteral("sm-1"));
        CHECK(stub.covers(QStringLiteral("sm-1")) == 1);
    }
    // A REAL IMAGE: lands once, and a later pass over the cached cover asks and re-renders nothing.
    {
        stub.answers[QStringLiteral("sm-3")] = CoverStub::Answer::Image;
        ArtView& v = newView(prefetch, key("sm-3"));
        v.render();
        CHECK(waitFor([&] { return v.landed >= 1; }));
        settleView(v);
        CHECK(stub.covers(QStringLiteral("sm-3")) == 1);
        CHECK(v.landed == 1);
        CHECK(v.renders == 2);
        CHECK(!coverOf(key("sm-3")).isEmpty());
    }
    // A FAILURE: not in a loop, and not remembered.
    {
        stub.answers[QStringLiteral("sm-4")] = CoverStub::Answer::ServerError;
        ArtView& v = newView(prefetch, key("sm-4"));
        v.render();
        CHECK(waitFor([&] { return stub.covers(QStringLiteral("sm-4")) >= 1; }));
        settleView(v);
        CHECK(stub.covers(QStringLiteral("sm-4")) == 1);
        CHECK(v.landed == 0);
        anotherPass(v, stub, QStringLiteral("sm-4"));
        CHECK(stub.covers(QStringLiteral("sm-4")) == 2);
    }
}

// THE RULE, AS A TABLE — including the case no live stub produces cheaply: a TIMEOUT, which reaches the
// client as a failed transfer with no status line at all, and must be Retry, never "no art".
static void testCoverAnswerRules()
{
    using A = CoverFetch::Answer;
    const QByteArray none;
    const QByteArray png = QByteArray::fromHex("89504e470d0a1a0a0000000d49484452");   // a PNG's first bytes
    CHECK(CoverFetch::classify(true, 200, none) == A::Absent);    // #370's empty image
    CHECK(CoverFetch::classify(true, 204, none) == A::Absent);
    CHECK(CoverFetch::classify(true, 200, png) == A::Image);
    CHECK(CoverFetch::classify(false, 404, none) == A::Absent);   // the server: "no such image"
    CHECK(CoverFetch::classify(false, 410, none) == A::Absent);
    CHECK(CoverFetch::classify(false, 0, none) == A::Retry);      // refused connection - or a TIMEOUT
    CHECK(CoverFetch::classify(false, 500, none) == A::Retry);
    CHECK(CoverFetch::classify(false, 503, none) == A::Retry);
    CHECK(CoverFetch::classify(false, 401, none) == A::Retry);
    CHECK(CoverFetch::classify(false, 200, png) == A::Retry);     // cut off mid-body: not a picture
    CHECK(CoverFetch::kTransferTimeoutMs > 0);

    // Subsonic's envelope layer, in both encodings. Bodies are built outside CHECK so no escape sequence ever
    // reaches a stringified condition (GCC reads some of those as universal character names).
    const QByteArray nfXml = "<subsonic-response status=\"failed\" version=\"1.16.1\">"
                             "<error code=\"70\" message=\"Cover art not found\"/></subsonic-response>";
    const QByteArray nfJson = "{\"subsonic-response\":{\"status\":\"failed\",\"version\":\"1.16.1\","
                              "\"error\":{\"code\":70,\"message\":\"Cover art not found\"}}}";
    const QByteArray nfPadded = QByteArray("  \r\n") + nfXml;
    const QByteArray authXml = "<subsonic-response status=\"failed\" version=\"1.16.1\">"
                               "<error code=\"40\" message=\"Wrong username or password\"/></subsonic-response>";
    const QByteArray okXml = "<subsonic-response status=\"ok\" version=\"1.16.1\"/>";
    const QByteArray svg = "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"1\" height=\"1\"/>";
    CHECK(Subsonic::coverAnswer(true, 200, nfXml) == A::Absent);
    CHECK(Subsonic::coverAnswer(true, 200, nfJson) == A::Absent);
    CHECK(Subsonic::coverAnswer(true, 200, nfPadded) == A::Absent);
    CHECK(Subsonic::coverAnswer(true, 200, authXml) == A::Retry);   // a refused credential is not "no art"
    CHECK(Subsonic::coverAnswer(true, 200, okXml) == A::Absent);    // a subsonic-response is never a picture
    CHECK(Subsonic::coverAnswer(true, 200, svg) == A::Image);       // markup that IS a picture stays one
    CHECK(Subsonic::coverAnswer(true, 200, png) == A::Image);
    CHECK(Subsonic::coverAnswer(true, 200, none) == A::Absent);
    CHECK(Subsonic::coverAnswer(false, 0, none) == A::Retry);       // the timeout shape
    CHECK(Subsonic::coverAnswer(false, 404, none) == A::Absent);
    CHECK(Subsonic::coverAnswer(false, 500, none) == A::Retry);
}

// NO CREDENTIAL IN THE NEW STATE. The Subsonic cover url carries the token and the salt, and a shelf's image
// url may be signed; "no art" is keyed on the album key, and this holds all three clients to that by scanning
// everything they now remember for every secret that actually went over the wire.
static void testCoverStateHoldsNoCredential(const CoverStub& stub, const QString& serverId)
{
    const QSet<QString> sub = SubsonicClient::instance().coversKnownMissing();
    const QSet<QString> jf  = JellyfinMusicClient::instance().coversKnownMissing();
    const QSet<QString> sm  = ServerMusicClient::instance().coversKnownMissing();

    // Not vacuous: each client remembers exactly what it was TOLD had no picture, and nothing it failed on.
    auto sKey = [&](int i) {
        return Subsonic::qualify(serverId, Subsonic::Kind::Album, QStringLiteral("al-%1").arg(i));
    };
    CHECK(sub.contains(sKey(1)) && sub.contains(sKey(4)) && sub.contains(sKey(5)));   // empty, "not found", 404
    CHECK(!sub.contains(sKey(2)) && !sub.contains(sKey(3)) && !sub.contains(sKey(6))); // image, 500, refused
    CHECK(sub.size() == 3);
    const QString jf2 = Jellyfin::qualify(QLatin1String(kJfServerId), QStringLiteral("jf-2"));
    const QString jf4 = Jellyfin::qualify(QLatin1String(kJfServerId), QStringLiteral("jf-4"));
    CHECK(jf.contains(jf2) && !jf.contains(jf4) && jf.size() == 1);
    const QString sm1 = ServerMusic::qualify(QLatin1String(kShelfId), ServerMusic::Kind::Album, QStringLiteral("sm-1"));
    const QString sm4 = ServerMusic::qualify(QLatin1String(kShelfId), ServerMusic::Kind::Album, QStringLiteral("sm-4"));
    CHECK(sm.contains(sm1) && !sm.contains(sm4) && sm.size() == 1);

    QByteArray scanned;
    for (const QSet<QString>* s : { &sub, &jf, &sm })
        for (const QString& k : *s) scanned += k.toUtf8() + '\n';
    CHECK(!scanned.isEmpty());

    // Every secret the stub actually received, harvested off the wire rather than recomputed.
    QSet<QByteArray> secrets{ QByteArray(kPassword), QByteArray(kJfToken) };
    int harvested = 0;
    for (const QString& t : stub.targets)
    {
        const QUrlQuery q{ QUrl(t) };
        for (const char* name : { "t", "s", "p" })
        {
            const QString v = q.queryItemValue(QLatin1String(name));
            if (!v.isEmpty()) { secrets.insert(v.toUtf8()); ++harvested; }
        }
    }
    CHECK(harvested > 0);                        // the scan is over real tokens, not over an empty list
    for (const QByteArray& s : secrets) CHECK(!scanned.contains(s));
    CHECK(!scanned.contains(QByteArray("&t=")));
    CHECK(!scanned.contains(QByteArray("/rest/")));
    CHECK(!scanned.contains(QByteArray("http")));
}

static void testCoverAnswers370()
{
    CoverStub stub;
    CHECK(stub.listen(QHostAddress::LocalHost, 0));
    if (!stub.isListening()) return;
    stub.root = QStringLiteral("http://127.0.0.1:%1").arg(stub.serverPort());
    {
        QImage img(4, 4, QImage::Format_RGB32);
        img.fill(Qt::darkCyan);
        QBuffer buf(&stub.imageBytes);
        buf.open(QIODevice::WriteOnly);
        img.save(&buf, "PNG");
    }
    CHECK(!stub.imageBytes.isEmpty());

    SubsonicServer srv;
    srv.name = QStringLiteral("Fixture"); srv.url = stub.root;
    srv.username = QLatin1String(kUser); srv.password = QLatin1String(kPassword); srv.allowPlainHttp = true;
    const QString serverId = SubsonicServerStore::add(srv);
    CHECK(!serverId.isEmpty());

    testSubsonicCoverAnswers(stub, serverId);
    testNextSessionAsksAgain(stub, serverId);
    testJellyfinCoverAnswers(stub);
    testServerMusicCoverAnswers(stub);
    testCoverStateHoldsNoCredential(stub, serverId);
    testCoverAnswerRules();
}

int main(int argc, char** argv)
{
    if (argc >= 4 && std::strcmp(argv[1], "cover-session") == 0)
    {
        QCoreApplication child(argc, argv);
        return coverSessionChild(QString::fromLocal8Bit(argv[2]), QString::fromLocal8Bit(argv[3]));
    }
    QCoreApplication app(argc, argv);

    // A real scanned library, from the shared fixtures — the same story probe_musicbrowse tells, and for the
    // same reason: the claims about local keys and local rendering are only worth anything against keys the
    // real scanner minted.
    QTemporaryDir tmp;
    if (!tmp.isValid()) { std::fprintf(stderr, "FAIL: no temp dir\n"); return 1; }
    writeFixtureLibrary(tmp.path());
    const MusicLibrary::Index local = MusicLibrary::buildIndex(MusicLibrary::scanFolder(tmp.path()));

    testIds();
    testNoLocalKeyParses(local);
    testAuth();
    testUrls();
    testEnvelope();
    testBothEncodingsAgree();
    testPayloads();
    testIndexShapes();
    testColdCacheAdopt();
    testServersLevel();
    testLocalUnchanged(local);
    // ---- increment 6: what the server already knows, and telling it what happened -------------------
    testPlaylistPayload();
    testPlaylistTracksKeepTheirOrder();
    testStarredPayload();
    testNewestPayload();
    testNewestPaging();
    testTwoServersCollidingSectionIds();
    testStarredUnionNeverReplaces();
    testScrobbleAndStarParams();
    testFate();
    testSectionLevels();
    testDoorsOnlyInsideAServer(local);
    testNoCredentialAnywhere();
    // ---- #370: what an empty or failed cover answer does to the level that asked --------------------
    testCoverAnswers370();

    if (g_fail) { std::fprintf(stderr, "%d check(s) failed\n", g_fail); return 1; }
    std::printf("SUBSONIC-OK\n");
    return 0;
}
