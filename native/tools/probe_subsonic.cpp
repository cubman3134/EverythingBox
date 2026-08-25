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
    // An unknown kind word likewise: a stale route must fail closed, not resolve as some other kind.
    CHECK(!Subsonic::isQualified(QStringLiteral("sub") + Subsonic::idSep() + A + Subsonic::idSep()
                                 + QStringLiteral("playlist") + Subsonic::idSep() + QStringLiteral("1")));
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

    if (g_fail) { std::fprintf(stderr, "%d check(s) failed\n", g_fail); return 1; }
    std::printf("SUBSONIC-OK\n");
    return 0;
}
