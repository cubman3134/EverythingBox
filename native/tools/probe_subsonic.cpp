// probe_subsonic — the Subsonic client's PURE half (issue #193, increment 5), driven with no server, no
// socket and no account.
//
// NO CREDENTIAL APPEARS ANYWHERE IN THIS FILE. Every password below is the literal string
// "probe-not-a-real-password", named so nobody can mistake it for one, and every host is a name that
// resolves nowhere. Nothing here opens a connection.
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
#include "Subsonic.h"
#include "MusicCatalogs.h"
#include "MusicFixtures.h"
#include "MusicLibrary.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QTemporaryDir>
#include <QUuid>

#include <cstdio>

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

int main(int argc, char** argv)
{
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
    testTwoServersCollidingSectionIds();
    testStarredUnionNeverReplaces();
    testScrobbleAndStarParams();
    testFate();
    testSectionLevels();
    testDoorsOnlyInsideAServer(local);
    testNoCredentialAnywhere();

    if (g_fail) { std::fprintf(stderr, "%d check(s) failed\n", g_fail); return 1; }
    std::printf("SUBSONIC-OK\n");
    return 0;
}
