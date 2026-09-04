// Headless check of THE TWO NEW MUSIC SUPPLIERS (issue #194, increment 3): every enabled Jellyfin server's
// music, and the EverythingBox server's music shelf, folded into the SAME merged library the local folder
// and every Subsonic server already fold into.
//
// The claim this probe exists to defend is the one #194 opens with: a person who owns an album in three
// places owns it once. So the sections below are paired the way probe_musicid's are, because the matcher is
// biased to refuse and a matcher biased to refuse passes every over-merge test by doing nothing:
//
//   * things that MUST merge across the new suppliers — a MusicBrainz id, a normalised artist+title, a
//     verdict the user recorded;
//   * things that MUST NOT — two records a year apart, two rows from the SAME supplier, an unqualifiable id;
//   * and the consequences of merging: which copy the preference picks, what a play from any copy banks
//     under, and what the picker is allowed to say about a copy's quality.
//
// It also pins the two protocol halves, because they are where a silent regression costs the most: a
// Jellyfin request that stops asking for `Fields=ProviderIds` still works perfectly and quietly loses every
// merge its ground truth, and an id family that stops being structurally distinct resolves one server's rows
// against another's.
//
// NO NETWORK, NO ADDON RUNTIME, NO SERVER: every unit under test takes bytes and strings as parameters.
// Prints MUSICSOURCES-OK on success; any failure prints MUSICSOURCES-FAIL <cond> and exits non-zero.
#include "Jellyfin.h"
#include "JellyfinMusic.h"
#include "MusicId.h"
#include "MusicMerge.h"
#include "MusicRemap.h"
#include "ServerMusic.h"
#include "Subsonic.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QString>
#include <QStringList>
#include <QVector>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "MUSICSOURCES-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

// Index::artist()/album() live in MusicLibrary.cpp, which pulls AudioTags and TagLib. This probe links
// neither, for the reason probe_musicid states: a probe that needs a tag reader is a probe nobody runs in a
// loop. So the two lookups are local.
static const MusicLibrary::Album* albumIn(const MusicLibrary::Index& idx, const QString& key)
{
    for (const MusicLibrary::Artist& a : idx.artists)
        for (const MusicLibrary::Album& b : a.albums)
            if (b.key == key) return &b;
    return nullptr;
}
static const MusicLibrary::Artist* artistIn(const MusicLibrary::Index& idx, const QString& key)
{
    for (const MusicLibrary::Artist& a : idx.artists) if (a.key == key) return &a;
    return nullptr;
}

// ---------------------------------------------------------------------------------------------------------
// Fixture bodies. Written as the servers write them, not as this app would like them written — a fixture
// tidied into the shape of its reader tests nothing.
// ---------------------------------------------------------------------------------------------------------
static const char* kJfArtists = R"({"Items":[
  {"Id":"a1","Name":"Pearl Jam","Type":"MusicArtist","ChildCount":3,
   "ProviderIds":{"MusicBrainzArtist":"83b9cbe7-9857-49e2-ab8e-b57b01038103"}},
  {"Id":"a2","Name":"Portishead","Type":"MusicArtist","ChildCount":1},
  {"Name":"A row with no id at all","Type":"MusicArtist"}
],"TotalRecordCount":3})";

static const char* kJfAlbums = R"({"Items":[
  {"Id":"b1","Name":"Ten","Type":"MusicAlbum","AlbumArtist":"Pearl Jam","ProductionYear":1991,
   "ChildCount":11,"RunTimeTicks":32220000000,
   "AlbumArtists":[{"Id":"a1","Name":"Pearl Jam"}],
   "ProviderIds":{"MusicBrainzAlbum":"rel-ten-1991","MusicBrainzReleaseGroup":"rg-ten",
                  "MusicBrainzAlbumArtist":"83b9cbe7-9857-49e2-ab8e-b57b01038103"}},
  {"Id":"b2","Name":"Ten","Type":"MusicAlbum","AlbumArtist":"Pearl Jam","ProductionYear":2009,
   "ChildCount":11,"AlbumArtists":[{"Id":"a1","Name":"Pearl Jam"}]}
]})";

static const char* kJfSongs = R"({"Items":[
  {"Id":"t2","Name":"Even Flow","Type":"Audio","Album":"Ten","AlbumId":"b1",
   "Artists":["Pearl Jam"],"IndexNumber":2,"ParentIndexNumber":1,"RunTimeTicks":29370000000,
   "MediaSources":[{"Container":"flac","Bitrate":1024000}]},
  {"Id":"t1","Name":"Once","Type":"Audio","Album":"Ten","AlbumId":"b1",
   "Artists":["Pearl Jam"],"IndexNumber":1,"ParentIndexNumber":1,"RunTimeTicks":23100000000,
   "MediaStreams":[{"Type":"Video","Codec":"mjpeg"},{"Type":"Audio","Codec":"flac","BitRate":1024000}]}
]})";

// The EverythingBox server's shelf, over the addon protocol's own envelope. Deliberately minimal in places:
// the second artist says nothing but its id and title, which is the case the header promises still works.
static const char* kShelfArtists = R"({"title":"Artists","items":[
  {"id":"ar-1","title":"Pearl Jam","type":"artist",
   "meta":{"albumCount":"2","musicBrainzArtistId":"83b9cbe7-9857-49e2-ab8e-b57b01038103"}},
  {"id":"ar-2","title":"Massive Attack"},
  {"id":"hdr","title":"Shuffle everything","type":"action"}
]})";

static const char* kShelfAlbums = R"({"items":[
  {"id":"al-1","title":"Vs.","type":"album",
   "meta":{"albumArtist":"Pearl Jam","year":1993,"trackCount":12,"durationSec":2820,
           "format":"mp3","bitrateKbps":320}},
  {"id":"al-2","title":"Ten","type":"album",
   "meta":{"albumArtist":"Pearl Jam","year":2009,"trackCount":11}}
]})";

static const char* kShelfSongs = R"({"items":[
  {"id":"tr-2","title":"Daughter","type":"track","url":"https://box.example/f/2?sig=REDACTED",
   "meta":{"artist":"Pearl Jam","track":4,"disc":1,"durationSec":235,"format":"mp3","bitrateKbps":320}},
  {"id":"tr-1","title":"Go","type":"track","url":"https://box.example/f/1?sig=REDACTED",
   "meta":{"artist":"Pearl Jam","track":1,"disc":1,"durationSec":158,"format":"mp3","bitrateKbps":320}}
]})";

// ---------------------------------------------------------------------------------------------------------
// Hand-built local and Subsonic indices, the same way probe_musicid builds them.
// ---------------------------------------------------------------------------------------------------------
static MusicLibrary::Album mkAlbum(const QString& key, const QString& artist, const QString& title,
                                   int year, int tracks, const QString& trackPathPrefix,
                                   const QString& mbRelease = QString(),
                                   const QString& mbGroup = QString())
{
    MusicLibrary::Album b;
    b.key = key; b.albumArtist = artist; b.title = title; b.year = year; b.trackCount = tracks;
    b.mbidRelease = mbRelease; b.mbidReleaseGroup = mbGroup;
    static const char* names[] = { "Once", "Even Flow", "Alive", "Go", "Animal", "Daughter" };
    for (int i = 1; i <= tracks; ++i)
    {
        MusicLibrary::IndexTrack t;
        t.path = trackPathPrefix + QString::number(i) + QStringLiteral(".flac");
        t.sourcePath = t.path;
        t.title = QString::fromLatin1(names[(i - 1) % 6]);
        t.albumKey = key; t.track = i;
        b.tracks.push_back(t);
    }
    return b;
}

static MusicLibrary::Artist mkArtist(const QString& key, const QString& name,
                                     const QVector<MusicLibrary::Album>& albums,
                                     const QString& mbid = QString())
{
    MusicLibrary::Artist a;
    a.key = key; a.name = name; a.mbid = mbid; a.albums = albums;
    a.albumCount = int(albums.size());
    for (const MusicLibrary::Album& b : albums) a.trackCount += int(b.tracks.size());
    return a;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // The override store PERSISTS. Redirected before anything reads it, so no run of this probe leaves a
    // verdict in the app's own ini for the next one to read.
    const QString ini = QDir::temp().filePath(QStringLiteral("eb_probe_musicsources.ini"));
    QFile::remove(ini);
    MusicId::setIniPathForTesting(ini);

    const QString jfServer = QStringLiteral("8f2c1d4b9a0e4f6c8b1d2e3f4a5b6c7d");   // a Jellyfin `Id`
    const QString jfOther  = QStringLiteral("11112222333344445555666677778888");
    const QString shelfId  = QStringLiteral("org.everythingbox.server");           // a connected server addon

    // =====================================================================================================
    // 1. THE JELLYFIN PAYLOADS
    // =====================================================================================================
    {
        bool ok = false;
        const QVector<JellyfinMusic::RemoteArtist> arts = JellyfinMusic::readArtists(kJfArtists, &ok);
        CHECK(ok);
        // Three rows in, two out: the one with no `Id` can never be qualified, so it is not a row.
        CHECK(arts.size() == 2);
        CHECK(arts.at(0).name == QStringLiteral("Pearl Jam"));
        CHECK(arts.at(0).albumCount == 3);
        // THE GROUND TRUTH ONLY ARRIVES BECAUSE THE REQUEST ASKED FOR IT — see section 2.
        CHECK(arts.at(0).musicBrainzArtistId == QStringLiteral("83b9cbe7-9857-49e2-ab8e-b57b01038103"));
        CHECK(arts.at(1).musicBrainzArtistId.isEmpty());   // absent, not guessed

        const QVector<JellyfinMusic::RemoteAlbum> albs = JellyfinMusic::readAlbums(kJfAlbums, &ok);
        CHECK(ok);
        CHECK(albs.size() == 2);
        CHECK(albs.at(0).artist == QStringLiteral("Pearl Jam"));
        CHECK(albs.at(0).artistId == QStringLiteral("a1"));
        CHECK(albs.at(0).year == 1991);
        CHECK(albs.at(0).songCount == 11);
        // TICKS ARE CONVERTED HERE AND NOWHERE ELSE: 32,220,000,000 ticks is 53:42.
        CHECK(albs.at(0).durationSec == 3222);
        // The RELEASE and the release GROUP are read into different fields. MusicId never compares one
        // against the other, and it can only keep that promise if they never share a slot.
        CHECK(albs.at(0).musicBrainzAlbumId == QStringLiteral("rel-ten-1991"));
        CHECK(albs.at(0).musicBrainzReleaseGroupId == QStringLiteral("rg-ten"));
        CHECK(albs.at(1).musicBrainzAlbumId.isEmpty());

        const QVector<JellyfinMusic::RemoteSong> songs = JellyfinMusic::readSongs(kJfSongs, &ok);
        CHECK(ok);
        CHECK(songs.size() == 2);
        CHECK(songs.at(0).track == 2 && songs.at(0).disc == 1);
        CHECK(songs.at(0).durationSec == 2937);
        // MediaSources first...
        CHECK(songs.at(0).format == QStringLiteral("FLAC"));
        CHECK(songs.at(0).bitrateKbps == 1024);
        // ...and MediaStreams as the fallback, reading the AUDIO stream. Taking the first stream would have
        // called this record MJPEG.
        CHECK(songs.at(1).format == QStringLiteral("FLAC"));
        CHECK(songs.at(1).bitrateKbps == 1024);

        // A body that is not an item envelope is NOT a server with no music. A proxy's error page must not
        // be able to empty a shelf.
        bool bad = true;
        JellyfinMusic::readArtists(QByteArray("<html>Bad Gateway</html>"), &bad);
        CHECK(!bad);
        bad = true;
        JellyfinMusic::readAlbums(QByteArray("{\"TotalRecordCount\":0}"), &bad);
        CHECK(!bad);
        // ...while a genuinely empty library parses fine and contributes nothing.
        bad = false;
        const QVector<JellyfinMusic::RemoteSong> none = JellyfinMusic::readSongs(QByteArray("{\"Items\":[]}"), &bad);
        CHECK(bad && none.isEmpty());
    }

    // =====================================================================================================
    // 2. THE REQUESTS. `Fields=ProviderIds` is what makes section 1's ground truth exist at all, and
    //    `MediaSources` is what makes the picker's quality line exist at all. Both are invisible from every
    //    layer above, so they are pinned here.
    // =====================================================================================================
    {
        const QString artists = JellyfinMusic::artistsPath(QStringLiteral("u-1"));
        CHECK(artists.startsWith(QStringLiteral("/Artists?")));
        CHECK(artists.contains(QStringLiteral("Fields=ProviderIds")));
        CHECK(artists.contains(QStringLiteral("userId=u-1")));

        const QString albums = JellyfinMusic::albumsPath(QStringLiteral("u-1"), QStringLiteral("a1"));
        CHECK(albums.startsWith(QStringLiteral("/Users/u-1/Items?")));
        CHECK(albums.contains(QStringLiteral("IncludeItemTypes=MusicAlbum")));
        // BY ALBUM ARTIST. `ArtistIds` returns every record this person merely plays on, and #194 would then
        // merge those onto the local library under the wrong artist.
        CHECK(albums.contains(QStringLiteral("AlbumArtistIds=a1")));
        CHECK(!albums.contains(QStringLiteral("&ArtistIds=")));
        CHECK(albums.contains(QStringLiteral("Fields=ProviderIds")));

        const QString songs = JellyfinMusic::songsPath(QStringLiteral("u-1"), QStringLiteral("b1"));
        CHECK(songs.contains(QStringLiteral("ParentId=b1")));
        CHECK(songs.contains(QStringLiteral("IncludeItemTypes=Audio")));
        CHECK(songs.contains(QStringLiteral("MediaSources")));

        // The stream url is the AUDIO endpoint, and it carries the token — which is precisely why it is
        // minted here and never stored. (The fixture value below is not a credential.)
        const QString url = JellyfinMusic::audioStreamUrl(QStringLiteral("https://jf.example"),
                                                          QStringLiteral("t1"), QStringLiteral("K"));
        CHECK(url.startsWith(QStringLiteral("https://jf.example/Audio/t1/stream?")));
        CHECK(url.contains(QStringLiteral("static=true")));
        CHECK(url.contains(QStringLiteral("api_key=K")));
        CHECK(JellyfinMusic::audioStreamUrl(QString(), QStringLiteral("t1"), QStringLiteral("K")).isEmpty());
        CHECK(JellyfinMusic::audioStreamUrl(QStringLiteral("https://jf.example"), QString(),
                                            QStringLiteral("K")).isEmpty());
    }

    // =====================================================================================================
    // 3. IDS. Every key a Jellyfin music row produces is #160's own `jf:<serverId>:<itemId>` — no second
    //    namespace — and the four key families in this app stay mutually unreadable.
    // =====================================================================================================
    {
        bool ok = false;
        const QVector<JellyfinMusic::RemoteArtist> arts = JellyfinMusic::readArtists(kJfArtists, &ok);
        MusicLibrary::Index jf = JellyfinMusic::indexOfArtists(jfServer, arts);
        CHECK(jf.artists.size() == 2);
        const QString aKey = jf.artists.at(0).key;
        CHECK(aKey == QStringLiteral("jf:") + jfServer + QStringLiteral(":a1"));
        CHECK(Jellyfin::parse(aKey).ok);
        CHECK(Jellyfin::serverOf(aKey) == jfServer);
        // An id from server A never resolves against server B: the server is IN the key.
        CHECK(Jellyfin::qualify(jfOther, QStringLiteral("a1")) != aKey);
        // ...and no other family reads it.
        CHECK(!Subsonic::isQualified(aKey));
        CHECK(!ServerMusic::isQualified(aKey));

        // An unqualifiable row is DROPPED, never emitted half-formed. A malformed server id is the case
        // that matters: it is what a partially written settings row looks like.
        QVector<JellyfinMusic::RemoteArtist> one;
        one.push_back(arts.at(0));
        CHECK(JellyfinMusic::indexOfArtists(QStringLiteral("not-a-server-id"), one).artists.isEmpty());

        // The ServerMusic family, round-tripped exactly — including a remote id that itself contains the
        // separator and a colon, which a section() split would truncate.
        const QString nasty = QStringLiteral("al:1") + ServerMusic::idSep() + QStringLiteral("x/y");
        const QString q = ServerMusic::qualify(shelfId, ServerMusic::Kind::Album, nasty);
        const ServerMusic::Ref r = ServerMusic::parse(q);
        CHECK(r.ok && r.sourceId == shelfId && r.remoteId == nasty);
        CHECK(r.kind == ServerMusic::Kind::Album);
        CHECK(ServerMusic::qualify(QString(), ServerMusic::Kind::Album, nasty).isEmpty());
        CHECK(ServerMusic::qualify(shelfId, ServerMusic::Kind::Album, QString()).isEmpty());
        CHECK(!Jellyfin::isQualified(q));
        CHECK(!Subsonic::isQualified(q));

        // NO LOCAL KEY CAN EVER PARSE AS ONE. The hostile case is a library whose album artist is literally
        // the prefix: MusicLibrary's album key is <artist><US>"t"<US><folded title>, and "t" is not one of
        // this family's three kind words.
        const QString localish = QStringLiteral("ebs") + ServerMusic::idSep() + QStringLiteral("t")
                               + ServerMusic::idSep() + QStringLiteral("ten");
        CHECK(!ServerMusic::isQualified(localish));
        const QString localish4 = localish + ServerMusic::idSep() + QStringLiteral("more");
        CHECK(!ServerMusic::isQualified(localish4));   // four fields, but "t" is still not a kind
        CHECK(!ServerMusic::isQualified(QStringLiteral("Pearl Jam")));
        CHECK(!ServerMusic::isQualified(QStringLiteral("C:/Music/Pearl Jam/Ten/01.flac")));
    }

    // =====================================================================================================
    // 4. THE SHELF PAYLOADS. Every `meta` key is optional, and the reader tolerates the string form of a
    //    number — a hand-assembled shelf writes "year": "1979" sooner or later, and reading that as 0 would
    //    silently disarm the year gate for that album.
    // =====================================================================================================
    {
        bool ok = false;
        const QVector<ServerMusic::RemoteArtist> arts = ServerMusic::readArtists(kShelfArtists, &ok);
        CHECK(ok);
        // Three rows in, two out: the "action" row is not an artist and is skipped rather than coerced.
        CHECK(arts.size() == 2);
        CHECK(arts.at(0).albumCount == 2);              // "2" as a string
        CHECK(arts.at(1).name == QStringLiteral("Massive Attack"));
        CHECK(arts.at(1).albumCount == 0 && arts.at(1).musicBrainzArtistId.isEmpty());

        const QVector<ServerMusic::RemoteAlbum> albs = ServerMusic::readAlbums(kShelfAlbums, &ok);
        CHECK(ok && albs.size() == 2);
        CHECK(albs.at(0).artist == QStringLiteral("Pearl Jam") && albs.at(0).year == 1993);
        CHECK(albs.at(0).format == QStringLiteral("MP3") && albs.at(0).bitrateKbps == 320);
        // The second says nothing about its format, and gets no badge rather than a guessed one.
        CHECK(albs.at(1).format.isEmpty() && albs.at(1).bitrateKbps == 0);

        const QVector<ServerMusic::RemoteSong> songs = ServerMusic::readSongs(kShelfSongs, &ok);
        CHECK(ok && songs.size() == 2);
        CHECK(songs.at(0).track == 4 && songs.at(1).track == 1);
        // The row's url is READ, so the client can hold it for this session — and, below, is proved not to
        // reach the index.
        CHECK(songs.at(0).url.startsWith(QStringLiteral("https://box.example/f/2")));

        bool bad = true;
        ServerMusic::readAlbums(QByteArray("<html>503</html>"), &bad);
        CHECK(!bad);

        // A TRACK'S URL IS NOT ITS IDENTITY. The index stores the qualified id; the url the shelf sent is
        // nowhere in it, which is what stops a credential-bearing string from being copied into a queue and
        // then persisted.
        MusicLibrary::Index shelf = ServerMusic::indexOfArtists(shelfId, arts);
        const QString arKey = shelf.artists.at(0).key;
        ServerMusic::fillArtistAlbums(shelf, shelfId, arKey, albs);
        const QString alKey = shelf.artists.at(0).albums.at(0).key;
        ServerMusic::fillAlbumTracks(shelf, shelfId, alKey, songs);
        const MusicLibrary::Album* b = albumIn(shelf, alKey);
        CHECK(b != nullptr);
        CHECK(b->tracks.size() == 2);
        // Ordered disc-then-track, so the shelf's own reply order does not decide what plays first.
        CHECK(b->tracks.at(0).title == QStringLiteral("Go"));
        for (const MusicLibrary::IndexTrack& t : b->tracks)
        {
            CHECK(ServerMusic::isQualified(t.path));
            CHECK(!t.path.contains(QStringLiteral("http")));
            CHECK(!t.sourcePath.contains(QStringLiteral("http")));
        }
    }

    // =====================================================================================================
    // 5. THE MERGE ACROSS FOUR SUPPLIERS.
    // =====================================================================================================
    //
    // local:     Ten (1991, MB release-group rg-ten), Vs. (1993)
    // subsonic:  Ten (1991)
    // jellyfin:  Ten (1991, rg-ten), Ten (2009) — the second is a different record and must stay one
    // shelf:     Vs. (1993), Ten (2009) — the second refuses against the local 1991 record on the year gate
    //
    // One artist, and the albums a person actually owns.
    MusicLibrary::Index local;
    {
        QVector<MusicLibrary::Album> albums;
        albums.push_back(mkAlbum(QStringLiteral("PJ\x1F""t\x1F""ten"), QStringLiteral("Pearl Jam"),
                                 QStringLiteral("Ten"), 1991, 3,
                                 QStringLiteral("C:/Music/Pearl Jam/Ten/0"), QString(),
                                 QStringLiteral("rg-ten")));
        albums.push_back(mkAlbum(QStringLiteral("PJ\x1F""t\x1F""vs"), QStringLiteral("Pearl Jam"),
                                 QStringLiteral("Vs."), 1993, 3,
                                 QStringLiteral("C:/Music/Pearl Jam/Vs/0")));
        local.artists.push_back(mkArtist(QStringLiteral("pearl jam"), QStringLiteral("Pearl Jam"), albums));
    }

    const QString subServer = QStringLiteral("6f9619ff-8b86-d011-b42d-00c04fc964ff");
    MusicLibrary::Index sub;
    {
        Subsonic::RemoteArtist ra; ra.id = QStringLiteral("7"); ra.name = QStringLiteral("Pearl Jam");
        ra.albumCount = 1;
        sub = Subsonic::indexOfArtists(subServer, { ra });
        Subsonic::RemoteAlbum rb; rb.id = QStringLiteral("70"); rb.name = QStringLiteral("Ten");
        rb.artist = QStringLiteral("Pearl Jam"); rb.songCount = 3; rb.year = 1991;
        Subsonic::fillArtistAlbums(sub, subServer, sub.artists.at(0).key, { rb });
    }

    MusicLibrary::Index jf;
    {
        bool ok = false;
        jf = JellyfinMusic::indexOfArtists(jfServer, JellyfinMusic::readArtists(kJfArtists, &ok));
        JellyfinMusic::fillArtistAlbums(jf, jfServer, jf.artists.at(0).key,
                                        JellyfinMusic::readAlbums(kJfAlbums, &ok));
        JellyfinMusic::fillAlbumTracks(jf, jfServer, jf.artists.at(0).albums.at(0).key,
                                       JellyfinMusic::readSongs(kJfSongs, &ok));
    }

    MusicLibrary::Index shelf;
    {
        bool ok = false;
        shelf = ServerMusic::indexOfArtists(shelfId, ServerMusic::readArtists(kShelfArtists, &ok));
        ServerMusic::fillArtistAlbums(shelf, shelfId, shelf.artists.at(0).key,
                                      ServerMusic::readAlbums(kShelfAlbums, &ok));
        ServerMusic::fillAlbumTracks(shelf, shelfId, shelf.artists.at(0).albums.at(0).key,
                                     ServerMusic::readSongs(kShelfSongs, &ok));
    }

    const QString localTen  = QStringLiteral("PJ\x1F""t\x1F""ten");
    const QString localVs   = QStringLiteral("PJ\x1F""t\x1F""vs");
    const QString subTen    = sub.artists.at(0).albums.at(0).key;
    const QString jfTen91   = jf.artists.at(0).albums.at(0).key;
    const QString jfTen09   = jf.artists.at(0).albums.at(1).key;
    const QString shelfVs   = shelf.artists.at(0).albums.at(0).key;
    const QString shelfTen  = shelf.artists.at(0).albums.at(1).key;

    QVector<MusicMerge::Source> srcs;
    srcs.push_back({ QString(), &local });
    srcs.push_back({ subServer, &sub });
    srcs.push_back({ jfServer,  &jf });
    srcs.push_back({ shelfId,   &shelf });

    {
        const MusicMerge::Merged m = MusicMerge::merge(srcs, QStringLiteral("local"));
        CHECK(m.active);

        // ONE ARTIST, not four. The Jellyfin and shelf rows carry the same MusicBrainz artist id as each
        // other; the local and Subsonic ones carry none and join on the normalised name.
        int pearlJamRows = 0;
        for (const MusicLibrary::Artist& a : m.idx.artists)
            if (MusicId::normalizeArtist(a.name) == MusicId::normalizeArtist(QStringLiteral("Pearl Jam")))
                ++pearlJamRows;
        CHECK(pearlJamRows == 1);
        // Massive Attack is on the shelf alone and must still be reachable — a merge that swallowed a
        // single-source artist would be the wrong-merge failure at its largest scale.
        CHECK(artistIn(m.idx, shelf.artists.at(1).key) != nullptr);

        // TEN merges across THREE suppliers and is keyed on the LOCAL copy, which the preference picked.
        const QStringList ten = m.albumInstances(localTen);
        CHECK(ten.size() == 3);
        CHECK(ten.first() == localTen);
        CHECK(ten.contains(subTen));
        CHECK(ten.contains(jfTen91));      // by MusicBrainz release GROUP, agreeing with the local tags
        CHECK(m.sourceOf.value(jfTen91) == jfServer);
        CHECK(m.sourceOf.value(subTen) == subServer);

        // ...AND THE 2009 REISSUES STAY THEIR OWN RECORDS, on BOTH new suppliers. The titles are
        // letter-for-letter equal and the artists match; only the year says they are different things, and
        // it is a GATE. This is the refusal the whole design leans towards: a wrong merge here would hide
        // one of two records the user owns, with nothing on screen to say why.
        CHECK(!ten.contains(jfTen09));
        CHECK(!ten.contains(shelfTen));
        // Both are still REACHABLE, which is the point: a refused merge costs a duplicate row, never a
        // record. They are rendered under the Jellyfin copy's key because that supplier comes first in the
        // order and neither is local — the preference could not be met, so the deterministic fallback
        // decided it (MusicId::pickAutoSource).
        CHECK(albumIn(m.idx, jfTen09) != nullptr);
        CHECK(albumIn(m.idx, shelfTen) == nullptr);

        // The two 2009 pressings — one on Jellyfin, one on the shelf — ARE each other's copy, and merge on
        // the normalised artist and title with no MusicBrainz id on either side. So the year gate above
        // separated two different records without costing the user the merge they wanted.
        const QStringList ten09 = m.albumInstances(jfTen09);
        CHECK(ten09.size() == 2);
        CHECK(ten09.contains(shelfTen));
        // TWO ROWS FROM THE SAME SUPPLIER NEVER MERGE, and the guard holds on the UNION rather than on the
        // pair — so the local group cannot smuggle both Jellyfin Tens in through a transitive chain either.
        CHECK(!ten09.contains(jfTen91));

        // VS. merges local + the EverythingBox server's shelf on the normalised artist and title, with no
        // MusicBrainz id anywhere in sight — the deal the local library already gets.
        const QStringList vs = m.albumInstances(localVs);
        CHECK(vs.size() == 2);
        CHECK(vs.contains(shelfVs));
        CHECK(m.sourceOf.value(shelfVs) == shelfId);
    }

    // A SUPPLIER THAT REPORTS NO YEAR BRIDGES TWO RECORDS THE GATE WOULD SEPARATE.
    //
    // This is a LIMIT, recorded rather than argued for, and it is inherited whole from increment 1: an
    // unknown year is compatible with everything (absence is not disagreement — MusicId.h), and merging is
    // transitive, so a copy carrying no year at all joins the 1991 record to the 2009 one that the year
    // gate had just held apart. It is pinned here because this increment is what makes it easy to hit: the
    // new suppliers are the ones most likely to report a year for some rows and not others, and a probe
    // that quietly arranged its fixtures around this would leave the next person to rediscover it in a
    // user's library. Changing it means changing the matcher, which #194 increment 3 deliberately does not.
    {
        MusicLibrary::Index a, b, c;
        a.artists.push_back(mkArtist(QStringLiteral("pearl jam"), QStringLiteral("Pearl Jam"),
            { mkAlbum(QStringLiteral("L\x1F""t\x1F""ten"), QStringLiteral("Pearl Jam"),
                      QStringLiteral("Ten"), 1991, 1, QStringLiteral("C:/M/a/0")) }));
        // The bridge: a server that gave us the record but not its year.
        Subsonic::RemoteArtist ra; ra.id = QStringLiteral("9"); ra.name = QStringLiteral("Pearl Jam");
        b = Subsonic::indexOfArtists(subServer, { ra });
        Subsonic::RemoteAlbum rb; rb.id = QStringLiteral("90"); rb.name = QStringLiteral("Ten");
        rb.artist = QStringLiteral("Pearl Jam");             // year 0 — unknown
        Subsonic::fillArtistAlbums(b, subServer, b.artists.at(0).key, { rb });
        JellyfinMusic::RemoteArtist ja; ja.id = QStringLiteral("z1"); ja.name = QStringLiteral("Pearl Jam");
        c = JellyfinMusic::indexOfArtists(jfServer, { ja });
        JellyfinMusic::RemoteAlbum jb; jb.id = QStringLiteral("z2"); jb.name = QStringLiteral("Ten");
        jb.artist = QStringLiteral("Pearl Jam"); jb.year = 2009;
        JellyfinMusic::fillArtistAlbums(c, jfServer, c.artists.at(0).key, { jb });

        const MusicMerge::Merged direct = MusicMerge::merge({ { QString(), &a }, { jfServer, &c } },
                                                            QStringLiteral("local"));
        // Without the bridge the gate holds: 1991 and 2009 are two records.
        CHECK(direct.albumInstances(QStringLiteral("L\x1F""t\x1F""ten")).size() == 1);
        const MusicMerge::Merged bridged = MusicMerge::merge(
            { { QString(), &a }, { subServer, &b }, { jfServer, &c } }, QStringLiteral("local"));
        // With it, all three are one row. Documented, not endorsed.
        CHECK(bridged.albumInstances(QStringLiteral("L\x1F""t\x1F""ten")).size() == 3);
    }

    // A SUPPLIER THAT HAS NOT ANSWERED CONTRIBUTES NOTHING AND BLOCKS NOTHING. This is the whole of the
    // "a slow server does not stall the library" rule at this layer: an unfetched supplier is a null index,
    // and the merged library is exactly what the ones that did answer say it is.
    {
        QVector<MusicMerge::Source> slow = srcs;
        slow.push_back({ jfOther, nullptr });
        MusicLibrary::Index empty;
        slow.push_back({ QStringLiteral("org.example.other"), &empty });
        const MusicMerge::Merged m = MusicMerge::merge(slow, QStringLiteral("local"));
        CHECK(m.active);
        CHECK(m.albumInstances(localTen).size() == 3);
        CHECK(m.albumInstances(localVs).size() == 2);
    }

    // =====================================================================================================
    // 6. THE PREFERENCE DECIDES WHICH COPY IS THE ROW — across the new suppliers exactly as across the old.
    // =====================================================================================================
    {
        // "any music server, in the order they were added" — Subsonic is first in the source order.
        const MusicMerge::Merged anyServer = MusicMerge::merge(srcs, MusicId::kPreferServer);
        CHECK(anyServer.albumInstances(subTen).first() == subTen);
        CHECK(albumIn(anyServer.idx, subTen) != nullptr);
        CHECK(albumIn(anyServer.idx, localTen) == nullptr);

        // "that Jellyfin server specifically".
        const MusicMerge::Merged jfFirst = MusicMerge::merge(srcs, jfServer);
        CHECK(jfFirst.albumInstances(jfTen91).first() == jfTen91);
        // Vs. is not on that server at all, so the preference cannot be met and the row falls back to the
        // local copy rather than to nothing. A merged row must always render under exactly one key.
        CHECK(jfFirst.albumInstances(localVs).first() == localVs);

        // "the EverythingBox server specifically" — Vs. now plays from the shelf, and Ten, which is not on
        // it, still renders.
        const MusicMerge::Merged shelfFirst = MusicMerge::merge(srcs, shelfId);
        CHECK(shelfFirst.albumInstances(shelfVs).first() == shelfVs);
        CHECK(shelfFirst.albumInstances(localTen).size() == 3);

        // An unrecognised preference — a server id synced from another device, which is exactly what the
        // stored key can contain — reads as "local" rather than as an error.
        const MusicMerge::Merged strange = MusicMerge::merge(srcs, QStringLiteral("a-server-i-removed"));
        CHECK(strange.albumInstances(localTen).first() == localTen);
    }

    // =====================================================================================================
    // 7. THE MANUAL OVERRIDE REACHES THE NEW SUPPLIERS WITH NO SCHEMA CHANGE. Automatic matching on messy
    //    tags will be wrong sometimes, and the escape hatch is what makes that tolerable — in BOTH
    //    directions, the negative one being the important half.
    // =====================================================================================================
    {
        // "These are not the same album": the Jellyfin copy of Ten is separated from the local one.
        MusicId::setAlbumOverride(QStringLiteral("Pearl Jam"), QStringLiteral("Ten"),
                                  QStringLiteral("Pearl Jam"), QStringLiteral("Ten"), false);
        const MusicMerge::Merged split = MusicMerge::merge(srcs, QStringLiteral("local"));
        CHECK(split.albumInstances(localTen).size() == 1);
        CHECK(albumIn(split.idx, jfTen91) != nullptr);
        MusicId::clearAlbumOverride(QStringLiteral("Pearl Jam"), QStringLiteral("Ten"),
                                    QStringLiteral("Pearl Jam"), QStringLiteral("Ten"));

        // "This IS the same album as...": the 2009 reissues — one on Jellyfin, one on the EverythingBox
        // server's shelf — joined to the local record on the user's say-so, across three suppliers the
        // matcher had deliberately kept apart. A verdict WINS OUTRIGHT: it beats the year gate that
        // separated them.
        MusicId::setAlbumOverride(QStringLiteral("Pearl Jam"), QStringLiteral("Ten"),
                                  QStringLiteral("Pearl Jam"), QStringLiteral("Ten"), true);
        const MusicMerge::Merged joined = MusicMerge::merge(srcs, QStringLiteral("local"));
        // One row per supplier: local, Subsonic, Jellyfin, shelf. Both Jellyfin Tens now match the local
        // one by verdict, but the SAME-SUPPLIER guard still holds, so only one of them can join the group
        // and the other stays its own row. That guard is the reason this is 4 and not 5.
        const QStringList all = joined.albumInstances(localTen);
        CHECK(all.size() == 4);
        CHECK(all.contains(subTen) && all.contains(shelfTen));
        CHECK(all.contains(jfTen91) != all.contains(jfTen09));   // exactly one of the two
        MusicId::clearAlbumOverride(QStringLiteral("Pearl Jam"), QStringLiteral("Ten"),
                                    QStringLiteral("Pearl Jam"), QStringLiteral("Ten"));
        const MusicMerge::Merged after = MusicMerge::merge(srcs, QStringLiteral("local"));
        CHECK(after.albumInstances(localTen).size() == 3);
    }

    // =====================================================================================================
    // 8. WHAT WAS BANKED FOLLOWS THE MERGED IDENTITY. Favourites, playtime, resume positions and scrobbles
    //    key on the key the merged row is RENDERED under, so a play from the Jellyfin copy has to bank
    //    under the same key as a play from the local copy of the same album. MusicRemap is what makes that
    //    true, and this is the cross-supplier case of it.
    // =====================================================================================================
    {
        const MusicMerge::Merged m = MusicMerge::merge(srcs, QStringLiteral("local"));
        // The local Ten is the primary; its Jellyfin twin's tracks must map onto the local ones.
        MusicRemap::AlbumGroup g;
        for (const QString& k : m.albumInstances(localTen))
        {
            const MusicLibrary::Album* b = (k == localTen)   ? albumIn(local, k)
                                         : (k == jfTen91)    ? albumIn(jf, k)
                                                             : albumIn(sub, k);
            MusicRemap::Instance inst;
            inst.key = k;
            if (b)
                for (const MusicLibrary::IndexTrack& t : b->tracks)
                    inst.tracks.push_back(MusicRemap::TrackId{ t.track, t.title, QString(), t.path });
            g.instances.push_back(inst);
        }
        const MusicRemap::Table tbl = MusicRemap::tableFor({ g });
        // The Jellyfin copy's two fetched tracks are matched by number onto the local record's tracks 1
        // and 2 — so a listen banked against the Jellyfin copy lands on the row the user sees.
        const QString jfT1 = Jellyfin::qualify(jfServer, QStringLiteral("t1"));
        const QString jfT2 = Jellyfin::qualify(jfServer, QStringLiteral("t2"));
        CHECK(tbl.map.value(jfT1) == QStringLiteral("C:/Music/Pearl Jam/Ten/01.flac"));
        CHECK(tbl.map.value(jfT2) == QStringLiteral("C:/Music/Pearl Jam/Ten/02.flac"));
        // The Subsonic copy has no track list yet. That is not an error and not a reason to do anything but
        // wait: nothing about it is in the table, so nothing of the user's is moved anywhere.
        CHECK(!tbl.map.contains(subTen));
        // The primary's own tracks self-map and are therefore ABSENT, never mapped to themselves.
        CHECK(!tbl.map.contains(QStringLiteral("C:/Music/Pearl Jam/Ten/01.flac")));
    }

    // =====================================================================================================
    // 9. THE PICKER'S QUALITY LINE. "Where sources report format/bitrate" — and only there.
    // =====================================================================================================
    {
        const MusicLibrary::Album* jfB = albumIn(jf, jfTen91);
        CHECK(jfB != nullptr);
        CHECK(MusicMerge::qualityBits(*jfB) == QStringList({ QStringLiteral("FLAC"),
                                                             QStringLiteral("1024 kbps") }));

        const MusicLibrary::Album* shB = albumIn(shelf, shelfVs);
        CHECK(shB != nullptr);
        CHECK(MusicMerge::qualityBits(*shB) == QStringList({ QStringLiteral("MP3"),
                                                             QStringLiteral("320 kbps") }));

        // A LOCAL copy reports neither field, and its format is derived from the path — which is exact, and
        // is what this level showed before the feature existed.
        const MusicLibrary::Album* loB = albumIn(local, localTen);
        CHECK(loB != nullptr);
        CHECK(MusicMerge::qualityBits(*loB) == QStringList({ QStringLiteral("FLAC") }));

        // A SUBSONIC copy reports no container through the API this app uses, so it claims NOTHING. A
        // picker that guessed here would be inventing the one distinction it exists to draw.
        const MusicLibrary::Album* suB = albumIn(sub, subTen);
        CHECK(suB != nullptr);
        CHECK(MusicMerge::qualityBits(*suB).isEmpty());

        // A remote key is never mistaken for a path. "jf:<server>:t1" has no extension and no separator;
        // reading one out of it would put a badge on a copy nobody measured.
        MusicLibrary::Album fake;
        MusicLibrary::IndexTrack ft;
        ft.path = ft.sourcePath = Jellyfin::qualify(jfServer, QStringLiteral("t1.mp3"));
        fake.tracks.push_back(ft);
        CHECK(MusicMerge::qualityBits(fake).isEmpty());

        // A DOT IN A FOLDER NAME IS NOT AN EXTENSION. An extensionless file under a folder called
        // "Live.79" would otherwise be given a format of "79/01" — the last dot is close enough to the end
        // to pass the length test and only its position relative to the last separator rules it out.
        MusicLibrary::Album odd;
        MusicLibrary::IndexTrack ot;
        ot.path = ot.sourcePath = QStringLiteral("C:/Music/Live.79/01");
        odd.tracks.push_back(ot);
        CHECK(MusicMerge::qualityBits(odd).isEmpty());

        // AN ALBUM WHOSE TRACKS DISAGREE CLAIMS NO FORMAT. "This copy is FLAC" is a claim about the whole
        // record, and one MP3 among the FLACs makes it false.
        MusicLibrary::Index mixed = JellyfinMusic::indexOfArtists(jfServer, { { QStringLiteral("a9"),
                                        QStringLiteral("Mixed"), QString(), 1 } });
        JellyfinMusic::RemoteAlbum mb;
        mb.id = QStringLiteral("b9"); mb.name = QStringLiteral("Mixed"); mb.artist = QStringLiteral("Mixed");
        JellyfinMusic::fillArtistAlbums(mixed, jfServer, mixed.artists.at(0).key, { mb });
        JellyfinMusic::RemoteSong s1, s2;
        s1.id = QStringLiteral("m1"); s1.title = QStringLiteral("A"); s1.track = 1;
        s1.format = QStringLiteral("FLAC"); s1.bitrateKbps = 900;
        s2.id = QStringLiteral("m2"); s2.title = QStringLiteral("B"); s2.track = 2;
        s2.format = QStringLiteral("MP3"); s2.bitrateKbps = 320;
        const QString mixedKey = mixed.artists.at(0).albums.at(0).key;
        JellyfinMusic::fillAlbumTracks(mixed, jfServer, mixedKey, { s1, s2 });
        const MusicLibrary::Album* mxB = albumIn(mixed, mixedKey);
        CHECK(mxB != nullptr && mxB->format.isEmpty() && mxB->bitrateKbps == 0);
        CHECK(MusicMerge::qualityBits(*mxB).isEmpty());
    }

    // Leave nothing behind (issue #42).
    QFile::remove(ini);

    if (failures == 0) { std::puts("MUSICSOURCES-OK"); return 0; }
    std::fprintf(stderr, "MUSICSOURCES: %d check(s) failed\n", failures);
    return 1;
}
