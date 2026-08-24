// Headless check of THEMED LEAF ROUTING — what Enter on a browse row does, on the layout this app is
// actually used through.
//
// WHY THIS PROBE EXISTS. Commit a92c6dd fixed three faults in the themed (Triple/XMB) layout, and every one
// of them was GREEN under probe_nav and probe_themeview both before and after the fix: the suite could not
// tell the broken build from the working one. All three were found by a person driving the app by hand,
// which is not a gate. The fault at the centre of them is a routing one and therefore statable headlessly:
//
//     a local leaf activated through the THEMED path must reach a player.
//
// The themed column does not call HomeView::activateItem. It opens an inline Play / Favorite /
// Add-to-playlist chooser, and the chooser's Play calls HomeView::playThemedLeaf — a second dispatch site
// that used to carry its own hand-written list of local kinds. A kind in one list and not the other fell
// through to resolvePlay, which has no local branch, and answered "Nothing to play" for a row the classic
// grid played perfectly. That had already happened to a music track (#74), a photo (#102) and an OPDS book
// (#146). browse::localLeafRoute is the one table both sites now read, and this probe pins it.
//
// WHAT IT PINS:
//   §1 THE THEMED ENTER FORK (browse::themedEnterFor). A container drills, a synthetic "_" row drills, an
//      "info" guidance row drills — and only a real leaf opens the chooser. The guidance arm is a92c6dd's
//      third fault: with it wrong, Enter on the sentence explaining an empty column offers Play / Favorite /
//      Download over a line of prose, and that Play can only ever say "Nothing to play".
//   §2 THE TABLE (browse::localLeafKinds / localLeafRoute). EVERY kind in the table routes to a player —
//      walked from the table itself, not from a list retyped here, because a probe with its own copy of the
//      list is the very thing the table replaced. Plus the key contract: an album key round-trips through
//      musicKeyOf including one containing ':', and an unusable row (no url, no key) answers NotLocal rather
//      than being claimed and dropped.
//   §3 THE END-TO-END CLAIM, over the REAL catalog builders — localLibraryCatalog, photosFolderCatalog,
//      musicAlbumCatalog, opdsCatalog, musicArtistsCatalog's empty-note row. Every row of every one of them
//      is chained themedEnterFor -> Chooser -> localLeafRoute -> a player (or Drill, for a container or
//      guidance row). Not hand-picked rows: the whole catalog, so a new row shape a builder starts emitting
//      is covered the day it appears.
//   §4 THE NEGATIVE. A remote addon's catalog row — which HAS a url — answers NotLocal, so the router
//      cannot swallow rows the stream resolve owns. Without this, §2 and §3 are both satisfied by a router
//      that says OpenFile to everything.
//   §5 WHAT "ADD THIS ROW TO THE QUEUE" MEANS (browse::queueTargetFor, issue #193 increment 2). The second
//      question a browse row now has to answer, decided beside the first for the same reason: the themed
//      inline chooser, the browse context menu and the classic right-click all draw these verbs, and three
//      readings of a mime is three answers waiting to drift. Also pins the split that makes both surfaces
//      necessary — a TRACK is claimed by the chooser (themedEnterFor says Chooser) and a RECORD can only be
//      claimed by the context menu (themedEnterFor says Drill, on every layout).
//
// Prints LEAFROUTE-OK on success; any failure prints LEAFROUTE-FAIL <cond> (line) and exits non-zero.
#include "LeafRoute.h"
#include "MusicCatalogs.h"
#include "SyntheticCatalogs.h"
#include "OpdsFeed.h"

#include <QCoreApplication>
#include <QString>
#include <cstdio>

static int g_fails = 0;

#define CHECK(cond)                                                                        \
    do {                                                                                   \
        if (!(cond)) { std::printf("LEAFROUTE-FAIL %s (%d)\n", #cond, __LINE__); ++g_fails; } \
    } while (0)

using browse::LeafPlay;
using browse::LeafRoute;
using browse::ThemedEnter;

namespace {

// A track row's Play, as the surface performs it: the chooser's Play -> playThemedLeaf -> the table.
// Returns the route so a caller can assert on both the verb and the key.
LeafRoute enterAndPlay(const MediaItem& it)
{
    if (browse::themedEnterFor(it.type, it.expandable) != ThemedEnter::Chooser) return {};
    return browse::localLeafRoute(it);
}

// One album, hand-built. The Index is hand-built ON PURPOSE here: what is under test is ROUTING, and
// probe_musicbrowse already owns the claim that a real scan of real tagged files produces this shape. The
// album key deliberately contains a ':' — an album titled "Vol. 1: Live" is the case a section()-based key
// reader truncates, and the key is what the surface hands to PlaybackSession.
const char* kAlbumKey = "the hollows\x1f" "vol. 1: live";

MusicLibrary::Index oneAlbumIndex()
{
    MusicLibrary::IndexTrack t1;
    t1.path = QStringLiteral("C:/music/Vol 1/01 Dawn.flac");
    t1.title = QStringLiteral("Dawn"); t1.artist = QStringLiteral("The Hollows");
    t1.disc = 1; t1.track = 1; t1.durationSec = 200;
    MusicLibrary::IndexTrack t2 = t1;
    t2.path = QStringLiteral("C:/music/Vol 1/02 Dusk.flac");
    t2.title = QStringLiteral("Dusk"); t2.track = 2;

    MusicLibrary::Album b;
    b.key = QString::fromLatin1(kAlbumKey);
    b.albumArtist = QStringLiteral("The Hollows");
    b.title = QStringLiteral("Vol. 1: Live");
    b.folder = QStringLiteral("C:/music/Vol 1");
    b.year = 2019; b.durationSec = 400;
    b.tracks << t1 << t2;

    MusicLibrary::Artist a;
    a.key = QStringLiteral("the hollows");
    a.name = QStringLiteral("The Hollows");
    a.trackCount = 2;
    a.albums << b;

    MusicLibrary::Index idx;
    idx.artists << a;
    idx.trackCount = 2; idx.albumCount = 1;
    return idx;
}

// No cover resolution: the default touches an extracted-art cache and a sibling-file lookup, and this probe
// has no business on the filesystem. probe_musicbrowse pins the artwork.
QString noCover(const MusicLibrary::Album&) { return QString(); }

// Every row of a catalog, chained the way the themed surface chains it. `wantChooser` is how many rows are
// expected to be leaves; the rest must Drill, and every leaf must reach a player. Returns the leaf count so
// a caller can assert a builder produced the rows it thinks it did (a builder that silently emits NOTHING
// would otherwise satisfy "every row routes" trivially).
int chainAll(const MediaCatalog& cat)
{
    int leaves = 0;
    for (const MediaItem& it : cat.items)
    {
        if (browse::themedEnterFor(it.type, it.expandable) == ThemedEnter::Drill) continue;
        ++leaves;
        const LeafRoute r = browse::localLeafRoute(it);
        if (!r.isLocal())
            std::printf("LEAFROUTE-FAIL themed leaf reaches no player: type=%s mime=%s title=%s\n",
                        qPrintable(it.type), qPrintable(it.mime), qPrintable(it.title)), ++g_fails;
    }
    return leaves;
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // ---- §1 The themed Enter fork ------------------------------------------------------------------------
    {
        // A container drills in-column whatever its type says.
        CHECK(browse::themedEnterFor(QStringLiteral("series"), true) == ThemedEnter::Drill);
        CHECK(browse::themedEnterFor(QStringLiteral("platform"), true) == ThemedEnter::Drill);
        // A synthetic row acts through the ordinary path (Playlists, a playlist, "New…", the Music levels).
        CHECK(browse::themedEnterFor(QStringLiteral("_playlists"), false) == ThemedEnter::Drill);
        CHECK(browse::themedEnterFor(QString::fromLatin1(browse::kMusicPlayAlbumType), false) == ThemedEnter::Drill);
        // A GUIDANCE row (a92c6dd fault 3). It is prose, not an item: activateItem refuses type "info", so
        // the ordinary path is a deliberate no-op, where the chooser would offer Play over a sentence.
        CHECK(browse::themedEnterFor(QStringLiteral("info"), false) == ThemedEnter::Drill);
        // ...and a real leaf, which is the ONLY thing that opens the chooser.
        CHECK(browse::themedEnterFor(QStringLiteral("movie"), false) == ThemedEnter::Chooser);
        CHECK(browse::themedEnterFor(QString::fromLatin1(browse::kMusicTrackType), false) == ThemedEnter::Chooser);
        CHECK(browse::themedEnterFor(QStringLiteral("photo"), false) == ThemedEnter::Chooser);
    }

    // ---- §2 The table ------------------------------------------------------------------------------------
    {
        const QVector<browse::LocalLeafKind>& kinds = browse::localLeafKinds();
        CHECK(!kinds.isEmpty());
        for (const browse::LocalLeafKind& k : kinds)
        {
            // Build the minimal row this kind claims, straight from its own table entry — so a kind ADDED to
            // the table is exercised here without this file being touched.
            MediaItem it;
            QString spelling = QString::fromLatin1(k.id);
            if (k.prefix) spelling += QStringLiteral("some-key");
            (k.field == browse::LocalLeafKind::Mime ? it.mime : it.type) = spelling;
            it.url = QStringLiteral("C:/some/file.bin");   // every non-keyed route needs a file to open

            const LeafRoute r = browse::localLeafRoute(it);
            CHECK(r.play == k.play);
            CHECK(r.isLocal());
            // A keyed kind hands its key on; a whole-match kind carries none.
            CHECK(k.prefix ? r.key == QStringLiteral("some-key") : r.key.isEmpty());
        }

        // The key is "everything after the prefix", never a section(':') — an album titled "Vol. 1: Live"
        // would be truncated at the colon by one, and the truncated key resolves to no album at all.
        MediaItem track;
        track.type = QString::fromLatin1(browse::kMusicTrackType);
        track.mime = QString::fromLatin1(browse::kMusicTrackPrefix) + QString::fromLatin1(kAlbumKey);
        track.url = QStringLiteral("C:/music/Vol 1/01 Dawn.flac");
        const LeafRoute tr = enterAndPlay(track);
        CHECK(tr.play == LeafPlay::MusicAlbum);
        CHECK(tr.key == QString::fromLatin1(kAlbumKey));
        CHECK(tr.key.contains(QLatin1Char(':')));   // the case a section() reader loses

        // An UNUSABLE row is not claimed. Answering OpenFile with no url would consume the row and open
        // nothing, which reads as Enter doing absolutely nothing; NotLocal leaves it the resolve it had.
        MediaItem noUrl;
        noUrl.mime = QString::fromLatin1(browse::kLocalVideoMime);
        CHECK(browse::localLeafRoute(noUrl).play == LeafPlay::NotLocal);
        MediaItem noKey;
        noKey.mime = QString::fromLatin1(browse::kMusicTrackPrefix);   // the prefix and nothing after it
        noKey.url = QStringLiteral("C:/music/orphan.flac");
        CHECK(browse::localLeafRoute(noKey).play == LeafPlay::NotLocal);
    }

    // ---- §3 The real builders, end to end ----------------------------------------------------------------
    // THE claim the suite was missing. Every row of every local builder, chained the way the themed surface
    // chains it: Enter -> the chooser -> Play -> a player.
    {
        // Local Library (#8/#73): scanned videos.
        QVector<LocalLibrary::VideoEntry> vids;
        { LocalLibrary::VideoEntry e; e.path = QStringLiteral("C:/lib/Arrival (2016).mkv");
          e.kind = LocalLibrary::Kind::Movie; e.title = QStringLiteral("Arrival"); e.year = 2016; vids << e; }
        { LocalLibrary::VideoEntry e; e.path = QStringLiteral("C:/lib/Show/S01E02.mkv");
          e.kind = LocalLibrary::Kind::Episode; e.show = QStringLiteral("Show"); e.season = 1; e.episode = 2;
          vids << e; }
        const MediaCatalog lib = browse::localLibraryCatalog(vids);
        CHECK(chainAll(lib) == 2);

        // Photos (#102): image tiles. Broken on the themed surface until the table existed.
        QVector<PhotoLibrary::PhotoEntry> pics;
        { PhotoLibrary::PhotoEntry e; e.path = QStringLiteral("C:/pics/trip/a.jpg");
          e.folder = QStringLiteral("C:/pics/trip"); pics << e; }
        { PhotoLibrary::PhotoEntry e; e.path = QStringLiteral("C:/pics/trip/b.png");
          e.folder = QStringLiteral("C:/pics/trip"); pics << e; }
        const MediaCatalog photos = browse::photosFolderCatalog(pics, QStringLiteral("C:/pics/trip"));
        CHECK(chainAll(photos) == 2);

        // Music (#74): an album's tracks, led by the synthetic "Play album" row (which DRILLS, so it is not
        // one of the two leaves).
        const MusicLibrary::Index idx = oneAlbumIndex();
        const MediaCatalog tracks =
            browse::musicAlbumCatalog(idx, QString::fromLatin1(kAlbumKey), noCover);
        CHECK(tracks.items.size() == 3);   // "Play album" + two tracks
        CHECK(chainAll(tracks) == 2);
        // ...and the tracks route to THIS album, not to their containing folder.
        for (const MediaItem& it : tracks.items)
            if (browse::themedEnterFor(it.type, it.expandable) == ThemedEnter::Chooser)
                CHECK(browse::localLeafRoute(it).key == QString::fromLatin1(kAlbumKey));

        // OPDS (#146): a book leaf and a sub-feed container in one shelf. Broken on the themed surface until
        // the table existed, and its route is NOT "open the url" — the acquisition href has to be fetched
        // with the catalog's own auth first.
        OpdsFeed feed;
        feed.title = QStringLiteral("Shelf");
        {
            OpdsEntry e; e.title = QStringLiteral("A Book"); e.id = QStringLiteral("urn:1");
            OpdsLink lk; lk.rel = QStringLiteral("http://opds-spec.org/acquisition");
            lk.href = QStringLiteral("https://books.example/1.epub");
            lk.type = QStringLiteral("application/epub+zip");
            e.acquisition << lk;
            feed.entries << e;
        }
        {
            OpdsEntry e; e.title = QStringLiteral("More"); e.id = QStringLiteral("urn:2");
            OpdsLink lk; lk.rel = QStringLiteral("subsection");
            lk.href = QStringLiteral("https://books.example/more.xml");
            lk.type = QStringLiteral("application/atom+xml;profile=opds-catalog");
            e.navigation << lk;
            feed.entries << e;
        }
        const MediaCatalog shelf = browse::opdsCatalog(feed);
        CHECK(shelf.items.size() == 2);
        CHECK(chainAll(shelf) == 1);       // the book is a leaf; the sub-feed drills
        for (const MediaItem& it : shelf.items)
            if (browse::themedEnterFor(it.type, it.expandable) == ThemedEnter::Chooser)
                CHECK(browse::localLeafRoute(it).play == LeafPlay::OpdsBook);

        // The GUIDANCE row a builder emits for an empty level (a92c6dd faults 2 and 3), from the real
        // builder rather than a hand-made row: it must DRILL, where it is inert, and never reach the chooser.
        const browse::MusicEmptyNote note{ QStringLiteral("No music folder chosen yet."),
                                           QStringLiteral("C:/music") };
        const MediaCatalog empty = browse::musicArtistsCatalog(MusicLibrary::Index{}, note, noCover);
        CHECK(empty.items.size() == 1);
        CHECK(empty.items[0].type == QStringLiteral("info"));
        CHECK(chainAll(empty) == 0);       // zero leaves: nothing here offers a Play
    }

    // ---- §4 The negative ---------------------------------------------------------------------------------
    // A remote addon's row. It HAS a url — a stream link — and it must still answer NotLocal, or the router
    // has claimed a row whose playback belongs to resolvePlay, and §2/§3 above are satisfied by a router
    // that says OpenFile to everything.
    {
        MediaItem remote;
        remote.type = QStringLiteral("movie");
        remote.mime = QStringLiteral("video/mp4");
        remote.url  = QStringLiteral("https://cdn.example/stream.mp4");
        remote.id   = QStringLiteral("tt1234567");
        CHECK(enterAndPlay(remote).play == LeafPlay::NotLocal);

        // A series container from the same addon: not a leaf at all, so it never reaches the table.
        MediaItem series;
        series.type = QStringLiteral("series");
        series.expandable = true;
        series.id = QStringLiteral("tt7654321");
        CHECK(browse::themedEnterFor(series.type, series.expandable) == ThemedEnter::Drill);
        CHECK(browse::localLeafRoute(series).play == LeafPlay::NotLocal);
    }

    // ---- §5 What "add this row to the queue" means (issue #193 increment 2) ------------------------------
    // The reach verbs need a SECOND answer about a browse row, beside "what does Enter do": can this row be
    // put in the queue, and as what. It is decided here rather than in the two menus that draw it, for the
    // same reason §2's table exists — two menus asking the same question of a mime is two answers waiting to
    // drift. Driven off the REAL catalog builders, so a row shape a builder starts emitting is covered.
    {
        using browse::QueueAdd;
        const MusicLibrary::Index idx = oneAlbumIndex();
        const MediaCatalog album = browse::musicAlbumCatalog(idx, QString::fromLatin1(kAlbumKey), noCover);
        CHECK(album.items.size() == 3);          // the "Play album" action row + two tracks

        int tracks = 0, albums = 0;
        for (const MediaItem& it : album.items)
        {
            const browse::QueueTarget t = browse::queueTargetFor(it);
            CHECK(t.ok());                       // every row of an album level is queueable as SOMETHING
            CHECK(t.albumKey == QString::fromLatin1(kAlbumKey));   // …including the ':' in the key, unsplit
            if (t.what == QueueAdd::Track)
            {
                ++tracks;
                // The claim that makes the verb not a silent no-op: the path it hands over is the row's own
                // url, which IS the string the index (and therefore the queue) holds — a plain path here, an
                // mpv EDL clip url on a cue album.
                CHECK(t.trackPath == it.url);
                CHECK(!t.trackPath.isEmpty());
                // A track is reached through the CHOOSER (the themed inline rows), so the two surfaces do
                // not both claim it: this row's Enter opens a chooser, and that is where its verbs live.
                CHECK(browse::themedEnterFor(it.type, it.expandable) == ThemedEnter::Chooser);
            }
            else
            {
                ++albums;
                CHECK(t.trackPath.isEmpty());    // a record has no ONE file
                // …and an album row DRILLS on every layout, which is exactly why it can never carry a
                // chooser row and why the browse context menu is the route that has to offer it.
                CHECK(browse::themedEnterFor(it.type, it.expandable) == ThemedEnter::Drill);
            }
        }
        CHECK(tracks == 2 && albums == 1);

        // The album row one level up (the artist's album list) is the OTHER row that names a record.
        const MediaCatalog artist = browse::musicArtistCatalog(idx, QStringLiteral("the hollows"), noCover);
        int albumRows = 0;
        for (const MediaItem& it : artist.items)
        {
            const browse::QueueTarget t = browse::queueTargetFor(it);
            if (it.type == QString::fromLatin1(browse::kMusicAlbumType))
            {
                ++albumRows;
                CHECK(t.what == QueueAdd::Album && t.albumKey == QString::fromLatin1(kAlbumKey));
            }
            else
            {
                // Play all / Shuffle all queue an ARTIST, which is a different verb with its own row — and
                // claiming them here would put "Add to queue" on a row that already is one.
                CHECK(!t.ok());
            }
        }
        CHECK(albumRows == 1);

        // ---- refusals: a music row that names nothing addable ----
        MediaItem noFile;                        // a track row with no file
        noFile.type = QString::fromLatin1(browse::kMusicTrackType);
        noFile.mime = QString::fromLatin1(browse::kMusicTrackPrefix) + QString::fromLatin1(kAlbumKey);
        CHECK(!browse::queueTargetFor(noFile).ok());
        MediaItem noKey;                         // …and one naming no album
        noKey.type = QString::fromLatin1(browse::kMusicTrackType);
        noKey.mime = QString::fromLatin1(browse::kMusicTrackPrefix);
        noKey.url  = QStringLiteral("C:/music/Vol 1/01 Dawn.flac");
        CHECK(!browse::queueTargetFor(noKey).ok());
        MediaItem noAlbumKey;                    // …and an album row naming no album
        noAlbumKey.type = QString::fromLatin1(browse::kMusicAlbumType);
        noAlbumKey.mime = QString::fromLatin1(browse::kMusicAlbumPrefix);
        CHECK(!browse::queueTargetFor(noAlbumKey).ok());

        // ---- the negative: nothing that is not local music is claimed ----
        // Without this every assertion above is satisfied by a function that says Track to everything, and
        // the verbs would appear on a film, a ROM and a comic.
        MediaItem movie;  movie.type = QStringLiteral("movie");
        movie.mime = QStringLiteral("video/mp4"); movie.url = QStringLiteral("https://cdn.example/s.mp4");
        CHECK(!browse::queueTargetFor(movie).ok());
        MediaItem photo;  photo.type = QStringLiteral("photo");
        photo.mime = QString::fromLatin1(browse::kPhotoMime);
        photo.url = QStringLiteral("C:/pics/a.jpg");
        CHECK(!browse::queueTargetFor(photo).ok());
        MediaItem localVid; localVid.mime = QString::fromLatin1(browse::kLocalVideoMime);
        localVid.url = QStringLiteral("C:/vid/a.mkv");
        CHECK(!browse::queueTargetFor(localVid).ok());
        MediaItem book;   book.type = QString::fromLatin1(browse::kOpdsBookType);
        book.url = QStringLiteral("https://opds.example/acq/1");
        CHECK(!browse::queueTargetFor(book).ok());
        MediaItem game;   game.type = QStringLiteral("game");
        game.url = QStringLiteral("C:/roms/nes/a.nes");
        CHECK(!browse::queueTargetFor(game).ok());
        MediaItem artistRow; artistRow.type = QString::fromLatin1(browse::kMusicArtistType);
        artistRow.mime = QString::fromLatin1(browse::kMusicArtistPrefix) + QStringLiteral("the hollows");
        CHECK(!browse::queueTargetFor(artistRow).ok());   // a container: it drills, and Play all is its verb
    }

    if (g_fails) { std::printf("LEAFROUTE: %d failure(s)\n", g_fails); return 1; }
    std::printf("LEAFROUTE-OK\n");
    return 0;
}
