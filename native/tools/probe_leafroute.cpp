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
//   §6 A STARRED LIVE TV CHANNEL ON THE ★ FAVORITES SHELF (issue #244). The producer that puts it there
//      (browse::liveTvFavoriteRows) and the route that opens it, together, because the shipped bug needed
//      both halves: the shelf produced no row for a channel at all, and a channel row carries no url — so
//      even once produced, every file route in the table would have refused it. Pins that a legacy
//      url-shaped favourite is NOT listed, that no row carries a `://` anywhere, and that the channel LIST's
//      own rows (which do have a url) are untouched by the new prefix.
//   §7 STARRING A MUSIC TRACK FROM A CLASSIC MENU (issue #297). Which rows offer Favorite / Remove
//      (browse::trackFavoriteFor + trackFavoriteVerb, the same reading queueTargetFor makes), the record they
//      write (the row's own id — its path — and type "track"), and that the press goes through the REAL store
//      with the love hook installed: it fires for FavoritesStore::toggle and does NOT fire for addFromSource.
//      That last pair is the silent failure — a favourite that lands on the shelf and never reaches the server.
//   §8 OPENING A ROW ON THE ★ FAVORITES SHELF (issue #364). The other half of §7: a starred track opened from
//      the shelf said "That favourite's source addon isn't available", because the shelf's open path
//      (HomeView::openFavorite, routed by browse::favoriteRouteFor) had no track arm and fell through to the
//      add-on lookup. Pins, over rows the shelf's own builder makes from records the REAL store holds: a local
//      track opens by its file as kind "audio"; a Subsonic track opens by its qualified id through the
//      qualified-track door; a moved file, a removed server and a supplier with no door each say their OWN
//      sentence and never the add-on one; an add-on's favourite — including one TYPED "track" — still routes
//      to its add-on and still reports it missing; the path, Steam and Epic arms are untouched; and routing
//      every row fires no love hook and leaves the store exactly as it was.
//   §9 ADD TO PLAYLIST AND DOWNLOAD ON A CLASSIC TRACK ROW (issue #365). Which rows the classic menus offer the
//      two verbs on (browse::trackMenuVerbsFor): Add to playlist on every library track — local and Subsonic,
//      asked of the real album builder's rows — and on an add-on's track; Download ONLY on a remote add-on's
//      track, the one kind whose download actually happens, and never on a library track in any add-on context.
//      Plus the shape the playlist verb is reached through (browse::queueOnRowCopy): not run inside the press,
//      run once a turn later, and run on the row as it was when pressed even after the list it came from has
//      been overwritten at that index and reallocated. That last clause is the P key's use-after-free.
//
// Prints LEAFROUTE-OK on success; any failure prints LEAFROUTE-FAIL <cond> (line) and exits non-zero.
#include "FavoriteRoute.h"
#include "JellyfinCatalogs.h"
#include "LeafRoute.h"
#include "MusicCatalogs.h"
#include "QueuedRowVerb.h"
#include "ServerMusic.h"
#include "Subsonic.h"
#include "SyntheticCatalogs.h"
#include "OpdsFeed.h"

#include <QCoreApplication>
#include <QHash>
#include <QString>
#include <QStringList>
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

    // ---- A JELLYFIN ITEM (#83): the local leaf that has no file, and must not be asked for one ---------
    // The kind is walked generically by the table loop in §2 like every other; this section states the two
    // things that are SPECIFIC to it and that a generic walk cannot see.
    {
        const QString srv = QStringLiteral("0123456789abcdef0123456789abcdef");
        const QString qualified = Jellyfin::qualify(srv, QStringLiteral("aabbccdd"));

        // 1. IT IS CLAIMED WITH NO URL. Every other file route refuses a row with no url, deliberately -
        // claiming one would open nothing. A Jellyfin row carries none BY DESIGN (the link is minted at
        // play time and never written into a row), so it has to be claimed anyway or the whole category
        // answers "Nothing to play" on both layouts.
        MediaItem it;
        it.type = QStringLiteral("movie");
        it.mime = QString::fromLatin1(browse::kJellyfinItemPrefix) + qualified;
        CHECK(it.url.isEmpty());
        const LeafRoute r = enterAndPlay(it);
        CHECK(r.play == LeafPlay::JellyfinItem);
        CHECK(r.isLocal());

        // 2. THE KEY IS THE WHOLE QUALIFIED ID, COLONS AND ALL. "jf:<32 hex>:<item>" has two of them
        // before the item half even begins, so any section(':') reader would hand back "jf" and route the
        // row at a server that does not exist.
        CHECK(r.key == qualified);
        CHECK(r.key.count(QLatin1Char(':')) >= 2);
        CHECK(Jellyfin::parse(r.key).ok);
        CHECK(Jellyfin::parse(r.key).serverId == srv);

        // An item id that itself contains a colon survives the round trip, for the same reason.
        MediaItem odd;
        odd.type = QStringLiteral("episode");
        const QString oddId = Jellyfin::qualify(srv, QStringLiteral("weird:id:with:colons"));
        odd.mime = QString::fromLatin1(browse::kJellyfinItemPrefix) + oddId;
        CHECK(enterAndPlay(odd).key == oddId);

        // A row carrying the prefix and nothing after it names no item: NOT claimed, so it falls through to
        // the resolve it would have taken rather than being consumed and dropped.
        MediaItem empty;
        empty.type = QStringLiteral("movie");
        empty.mime = QString::fromLatin1(browse::kJellyfinItemPrefix);
        CHECK(browse::localLeafRoute(empty).play == LeafPlay::NotLocal);

        // ...and it is not a music row, so it carries no queue verbs. (#193's question, asked of #83's row.)
        CHECK(!browse::queueTargetFor(it).ok());
    }

    // ---- §6 A STARRED LIVE TV CHANNEL ON THE ★ FAVORITES SHELF (issue #244) ----------------------------
    // The shelf is an intersection of the level you are standing on with the favourites store, and a channel
    // is starred from a folder INSIDE the video catalogue rather than from the catalogue page — so the
    // intersection was empty by construction and a themed layout showed a starred channel nowhere at all.
    // browse::liveTvFavoriteRows is the producer that closes it. This section pins the producer AND the
    // route, because the bug needed both: a row nothing produced, and (had it been produced) a row with no
    // url, which every file route in the table above refuses.
    {
        const QString idA = QStringLiteral("livetv:bbc.one.uk");          // the tvg-id spelling
        const QString idB = QStringLiteral("livetv:name:film four hd");   // the name spelling — two colons

        auto chanFav = [](const QString& id, const QString& title) {
            FavoriteItem f;
            f.itemId = id; f.path = id;                   // the identity in BOTH, exactly as #203 files it
            f.title = title; f.type = QStringLiteral("livetv"); f.kind = QStringLiteral("livetv");
            return f;
        };

        // A MOVIE favourite and a starred LOCAL GAME sit in the same store. Neither may be drawn as a
        // channel: the producer's filter is on `type`, not on the id's shape.
        FavoriteItem movie;
        movie.itemId = QStringLiteral("tt0816692"); movie.title = QStringLiteral("Interstellar");
        movie.type = QStringLiteral("movie");
        FavoriteItem game;
        game.itemId = QStringLiteral("C:/roms/snes/Chrono Trigger.sfc"); game.type = QStringLiteral("game");
        game.path = game.itemId; game.kind = QStringLiteral("game");

        // A LEGACY row: an id that is still a url, which is what a build before #203 wrote. It has no
        // identity to resolve, so the shelf — whose whole contract is "resolved at open, never a stale url"
        // — must not list it. It is ALSO the row that must never be handed around, which is why this is the
        // assertion that matters most in this section.
        FavoriteItem legacy = chanFav(QStringLiteral("livetv:http://iptv.example.test/live/abc/def/12.ts"),
                                      QStringLiteral("Legacy Channel"));

        const QList<FavoriteItem> favs{ movie, chanFav(idA, QStringLiteral("BBC One")), game,
                                        legacy, chanFav(idB, QStringLiteral("Film4 HD")) };
        const QVector<MediaItem> rows = browse::liveTvFavoriteRows(favs);

        // 6a. THE PRODUCER: the two real channels, in the store's own order, and nothing else.
        CHECK(rows.size() == 2);
        if (rows.size() == 2)
        {
            CHECK(rows.at(0).id == idA);
            CHECK(rows.at(1).id == idB);
            CHECK(rows.at(0).title == QStringLiteral("BBC One"));
            // NO URL, on purpose. This is the field the shelf must not carry and the reason the row needs a
            // route of its own — a file route would claim it and open nothing.
            CHECK(rows.at(0).url.isEmpty());
            CHECK(rows.at(1).url.isEmpty());
            // ...and not one byte of the legacy row's url is anywhere in what the shelf produces.
            for (const MediaItem& r : rows)
            {
                CHECK(!r.id.contains(QStringLiteral("://")));
                CHECK(!r.mime.contains(QStringLiteral("://")));
                CHECK(!r.url.contains(QStringLiteral("://")));
            }

            // 6b. THE ROUTE, on BOTH surfaces — the chooser opens (it is a leaf, not a drill) and Play
            // reaches a player. This is the chain the themed layout takes, and the one #244 had no row for.
            for (const MediaItem& r : rows)
            {
                const LeafRoute lr = enterAndPlay(r);
                CHECK(lr.play == LeafPlay::LiveTvChannel);
                CHECK(lr.isLocal());
            }
            // The key is the WHOLE identity, colons and all. "livetv:name:film four hd" has two of them
            // before the name even begins, so any section(':') reader would hand back "livetv" and resolve
            // nothing — the same trap #83's qualified id sets one section up.
            CHECK(enterAndPlay(rows.at(0)).key == idA);
            CHECK(enterAndPlay(rows.at(1)).key == idB);
            CHECK(enterAndPlay(rows.at(1)).key.count(QLatin1Char(':')) == 2);
            CHECK(LiveTvIdentity::isLiveTvId(enterAndPlay(rows.at(1)).key));

            // ...and a channel is not a music row, so it carries no queue verbs.
            CHECK(!browse::queueTargetFor(rows.at(0)).ok());
        }

        // 6c. A row carrying the prefix and nothing after it names no channel: NOT claimed, so it falls
        // through to the resolve it would have taken rather than being consumed and dropped.
        MediaItem empty;
        empty.type = QStringLiteral("livetv");
        empty.mime = QString::fromLatin1(LiveTvIdentity::kLiveTvChannelPrefix);
        CHECK(browse::localLeafRoute(empty).play == LeafPlay::NotLocal);

        // 6d. THE NEIGHBOURING SURFACE IS UNTOUCHED. A row from the CHANNEL LIST carries mime "livetv" and a
        // real url, and opens through that url; the new prefix must not claim it, or #75's list would start
        // re-resolving every channel it already holds a link for.
        M3uEntry e;
        e.tvgId = QStringLiteral("bbc.one.uk"); e.title = QStringLiteral("BBC One");
        e.url = QStringLiteral("http://iptv.example.test/live/abc/def/12.ts");
        const MediaCatalog chans = browse::liveTvChannelsCatalog(QStringLiteral("Src"), { e }, {});
        for (const MediaItem& r : chans.items)
            CHECK(browse::localLeafRoute(r).play != LeafPlay::LiveTvChannel);

        // 6e. An EMPTY store produces no rows at all — the shelf's gate asks this exact question, and a
        // producer that invented a row would put an "★ Favorites" shelf on every video root.
        CHECK(browse::liveTvFavoriteRows({}).isEmpty());
        CHECK(browse::liveTvFavoriteRows({ movie, game }).isEmpty());
    }

    // ---- §7 STARRING A MUSIC TRACK FROM A CLASSIC MENU (issue #297) ---------------------------------------
    // The classic layout's two menus on a track row offered the queue verbs and nothing else, so a track could
    // be starred only from the themed chooser — and, since #193 increment 6, that was the only door to the
    // server star too. The star is sent by FavoritesStore's love hook, not by a button, so what the classic
    // verb has to get right is (a) which rows offer it and what it says, (b) the record it writes, and (c)
    // that it goes through add(), which fires the hook, and never addFromSource(), which deliberately does
    // not. (c) is the one that fails silently: a favourite lands, the shelf shows it, and the server never
    // hears. So this section drives the REAL store with a hook installed and watches it.
    {
        using browse::TrackFavVerb;
        const MusicLibrary::Index idx = oneAlbumIndex();
        const MediaCatalog album = browse::musicAlbumCatalog(idx, QString::fromLatin1(kAlbumKey), noCover);

        // 7a. EVERY TRACK ROW OFFERS IT, as the record the themed chooser writes — and the row that is not a
        // track (the "Play album" action row at the top) does not. Asked of the real builder's rows.
        int offered = 0;
        MediaItem dawn;
        for (const MediaItem& it : album.items)
        {
            const FavoriteItem f = browse::trackFavoriteFor(it);
            const bool isTrack = browse::queueTargetFor(it).what == browse::QueueAdd::Track;
            CHECK(f.itemId.isEmpty() == !isTrack);   // the queue's reading and this one agree, row for row
            if (!isTrack)
            {
                CHECK(browse::trackFavoriteVerb(f, false) == TrackFavVerb::None);
                CHECK(browse::trackFavoriteVerb(f, true) == TrackFavVerb::None);
                continue;
            }
            ++offered;
            // THE ROW'S OWN ID, which for a track is its path: the id adoptStarredFavourites files a server's
            // star under and the id the love hook recovers the tags by. Another id and the row's heart could
            // not find its own favourite.
            CHECK(f.itemId == it.id);
            CHECK(f.itemId == it.url);
            // "track", because that is the ONE type the love hook acts on (MainWindow's setLoveHook).
            CHECK(f.type == QLatin1String("track"));
            CHECK(f.type == QString::fromLatin1(browse::kMusicTrackType));
            CHECK(f.title == it.title);
            CHECK(f.subtitle == it.subtitle);
            CHECK(f.thumbnailUrl == it.thumbnailUrl);
            // The themed chooser's generic arm stamps no path/kind/system for a track; neither does this, or one
            // track would be two differently shaped records depending on which layout starred it.
            CHECK(f.path.isEmpty() && f.kind.isEmpty() && f.system.isEmpty() && f.addonId.isEmpty());
            CHECK(browse::trackFavoriteVerb(f, false) == TrackFavVerb::Add);
            CHECK(browse::trackFavoriteVerb(f, true) == TrackFavVerb::Remove);
            // Picked by FILE, not title: the row's title is numbered ("1. Dawn") in an album's list.
            if (it.url == QStringLiteral("C:/music/Vol 1/01 Dawn.flac")) dawn = it;
        }
        CHECK(offered == 2);
        // ...and the id is IndexTrack::path verbatim, which is the key adoptStarredFavourites uses.
        CHECK(dawn.id == QStringLiteral("C:/music/Vol 1/01 Dawn.flac"));

        // 7b. NOTHING THAT IS NOT A TRACK OFFERS IT. Without this, 7a is satisfied by a function that offers
        // Favorite on every row, and the classic menu would grow a second, differently shaped star on a film.
        const MediaCatalog artist = browse::musicArtistCatalog(idx, QStringLiteral("the hollows"), noCover);
        CHECK(!artist.items.isEmpty());
        for (const MediaItem& it : artist.items)                 // the album row, Play all, Shuffle all
            CHECK(browse::trackFavoriteFor(it).itemId.isEmpty());
        MediaItem movie;  movie.type = QStringLiteral("movie"); movie.id = QStringLiteral("tt0816692");
        movie.mime = QStringLiteral("video/mp4"); movie.url = QStringLiteral("https://cdn.example/s.mp4");
        MediaItem photo;  photo.type = QStringLiteral("photo"); photo.mime = QString::fromLatin1(browse::kPhotoMime);
        photo.url = QStringLiteral("C:/pics/a.jpg"); photo.id = photo.url;
        MediaItem game;   game.type = QStringLiteral("game"); game.url = QStringLiteral("C:/roms/nes/a.nes");
        game.id = game.url;
        MediaItem book;   book.type = QString::fromLatin1(browse::kOpdsBookType);
        book.url = QStringLiteral("https://opds.example/acq/1"); book.id = book.url;
        MediaItem localVid; localVid.mime = QString::fromLatin1(browse::kLocalVideoMime);
        localVid.url = QStringLiteral("C:/vid/a.mkv"); localVid.id = localVid.url;
        MediaItem artistRow; artistRow.type = QString::fromLatin1(browse::kMusicArtistType);
        artistRow.mime = QString::fromLatin1(browse::kMusicArtistPrefix) + QStringLiteral("the hollows");
        MediaItem jf;     jf.type = QStringLiteral("movie"); jf.id = QStringLiteral("jfid");
        jf.mime = QString::fromLatin1(browse::kJellyfinItemPrefix)
                  + Jellyfin::qualify(QStringLiteral("0123456789abcdef0123456789abcdef"), QStringLiteral("aa"));
        MediaItem noFile; noFile.type = QString::fromLatin1(browse::kMusicTrackType);   // a track naming no file
        noFile.mime = QString::fromLatin1(browse::kMusicTrackPrefix) + QString::fromLatin1(kAlbumKey);
        noFile.id = QStringLiteral("C:/music/gone.flac");
        for (const MediaItem& it : { movie, photo, game, book, localVid, artistRow, jf, noFile })
        {
            const FavoriteItem f = browse::trackFavoriteFor(it);
            CHECK(f.itemId.isEmpty());
            CHECK(browse::trackFavoriteVerb(f, false) == TrackFavVerb::None);
        }

        // 7c. THROUGH THE STORE, AND THE LOVE HOOK FIRES. The hook is the real seam MainWindow installs; the
        // probe installs a recorder in its place and presses the verb the way both classic menus press it.
        struct Love { FavoriteItem f; bool loved; };
        QVector<Love> loves;
        FavoritesStore::setLoveHook([&loves](const FavoriteItem& f, bool loved) { loves.push_back({ f, loved }); });
        const FavoriteItem fav = browse::trackFavoriteFor(dawn);
        CHECK(!FavoritesStore::isFavorite(dawn.id));
        CHECK(FavoritesStore::toggle(fav));                     // "Favorite"
        CHECK(FavoritesStore::isFavorite(dawn.id));             // the row's own id finds it
        int landed = 0;
        for (const FavoriteItem& s : FavoritesStore::list())
            if (s.itemId == dawn.id && s.type == QLatin1String("track") && s.title == dawn.title) ++landed;
        CHECK(landed == 1);
        CHECK(loves.size() == 1);
        if (loves.size() == 1)
        {
            CHECK(loves.at(0).loved);
            CHECK(loves.at(0).f.itemId == dawn.id);
            CHECK(loves.at(0).f.type == QLatin1String("track"));
        }
        // Standing on it again, the menu now says the other thing.
        CHECK(browse::trackFavoriteVerb(browse::trackFavoriteFor(dawn), FavoritesStore::isFavorite(dawn.id))
              == TrackFavVerb::Remove);
        CHECK(!FavoritesStore::toggle(fav));                    // "Remove from Favorites"
        CHECK(!FavoritesStore::isFavorite(dawn.id));
        CHECK(loves.size() == 2);
        if (loves.size() == 2)
        {
            CHECK(!loves.at(1).loved);                          // the un-star reaches the server too
            CHECK(loves.at(1).f.itemId == dawn.id);
            CHECK(loves.at(1).f.type == QLatin1String("track"));
        }
        CHECK(browse::trackFavoriteVerb(browse::trackFavoriteFor(dawn), FavoritesStore::isFavorite(dawn.id))
              == TrackFavVerb::Add);

        // 7d. ...AND NOT FOR addFromSource. The same record arriving as a server's own star stays quiet — that
        // is the whole reason the second entry point exists — which is what makes 7c a real distinction and
        // not a hook that fires for everything.
        loves.clear();
        FavoritesStore::addFromSource(fav);
        CHECK(FavoritesStore::isFavorite(dawn.id));
        CHECK(loves.isEmpty());
        // A server's star shows on the classic row as Remove, and removing it there DOES tell the server.
        CHECK(browse::trackFavoriteVerb(browse::trackFavoriteFor(dawn), FavoritesStore::isFavorite(dawn.id))
              == TrackFavVerb::Remove);
        CHECK(!FavoritesStore::toggle(fav));
        CHECK(loves.size() == 1 && !loves.at(0).loved);

        // 7e. A record naming nothing (what a non-track row resolves to) is refused and fires nothing.
        loves.clear();
        const FavoriteItem none;
        CHECK(!FavoritesStore::toggle(none));
        CHECK(loves.isEmpty());
        CHECK(FavoritesStore::list().isEmpty());
        FavoritesStore::setLoveHook({});
    }

    // ---- §8 OPENING A ROW ON THE ★ FAVORITES SHELF (issue #364) ------------------------------------------
    // §7 puts a track on the shelf; this is what pressing it there does. The records are put in the REAL store
    // and read back out of it, the rows are built by the shelf's own builder (favoriteShelfRow), and each is
    // routed the way HomeView::openFavorite routes it — so "the record the writers write" and "the record the
    // shelf reads" are one record here, as they are in the app.
    {
        using browse::FavoriteOpen;
        using browse::FavoriteRoute;

        const QString dawnPath = QStringLiteral("C:/music/Vol 1/01 Dawn.flac");
        const QString gonePath = QStringLiteral("C:/music/Moved Away/03 Gone.flac");
        // A cue track's id is mpv's clip url of the shared file (MusicLibrary::IndexTrack::path).
        const QString clipUrl  = QStringLiteral("edl://%27%C:/music/Live/live.flac,start=12.5,length=200");
        const QString srvHere  = QStringLiteral("3f2b8c1e-6a4d-4e0b-9a51-2c7d8e9f0a1b");
        const QString srvGone  = QStringLiteral("9d0c6b7a-1e2f-4a3b-8c4d-5e6f7a8b9c0d");
        const QString subHere  = Subsonic::qualify(srvHere, Subsonic::Kind::Track, QStringLiteral("tr-42"));
        const QString subGone  = Subsonic::qualify(srvGone, Subsonic::Kind::Track, QStringLiteral("tr-7"));
        const QString jfTrack  = Jellyfin::qualify(QStringLiteral("0123456789abcdef0123456789abcdef"),
                                                   QStringLiteral("song1"));
        const QString ebsTrack = ServerMusic::qualify(QStringLiteral("shelf-1"), ServerMusic::Kind::Track,
                                                      QStringLiteral("t9"));
        // FIXTURE SANITY. An id that failed to qualify would be read as a local path, and every server case
        // below would be quietly testing the local arm instead.
        CHECK(Subsonic::isQualified(subHere) && Subsonic::serverOf(subHere) == srvHere);
        CHECK(Subsonic::isQualified(subGone) && Subsonic::serverOf(subGone) == srvGone);
        CHECK(Jellyfin::isQualified(jfTrack) && !Subsonic::isQualified(jfTrack));
        CHECK(ServerMusic::isQualified(ebsTrack) && !Subsonic::isQualified(ebsTrack));
        for (const QString& local : { dawnPath, gonePath, clipUrl })
            CHECK(!Subsonic::isQualified(local) && !Jellyfin::isQualified(local) && !ServerMusic::isQualified(local));

        // The world the router asks. `asked` records every file question, so a server id can be shown never
        // to have been treated as a path.
        const QStringList files   = { dawnPath };
        const QStringList servers = { srvHere };
        const QStringList sources = { QStringLiteral("com.example.films"), QStringLiteral("com.example.radio") };
        QStringList asked;
        browse::FavoriteWorld world;
        world.fileExists  = [&](const QString& f) { asked << f; return files.contains(f); };
        world.serverKnown = [&](const QString& s) { return servers.contains(s); };
        world.sourceKnown = [&](const QString& a) { return sources.contains(a); };

        // THE LOCAL TRACK, exactly as the classic menu (#297) and the themed chooser write it — asked of the
        // real album builder's row, not typed out here.
        const MusicLibrary::Index idx = oneAlbumIndex();
        const MediaCatalog album = browse::musicAlbumCatalog(idx, QString::fromLatin1(kAlbumKey), noCover);
        FavoriteItem localFav;
        for (const MediaItem& it : album.items)
            if (it.url == dawnPath) localFav = browse::trackFavoriteFor(it);
        CHECK(localFav.itemId == dawnPath && localFav.type == QLatin1String("track"));
        // The record #364 is about: no path, no kind, no add-on. A route that needed any of them is a route
        // that cannot open the stars people already have.
        CHECK(localFav.path.isEmpty() && localFav.kind.isEmpty() && localFav.addonId.isEmpty());

        // Every other track record in adoptStarredFavourites' shape: id, title, artist, type "track".
        auto trackRec = [](const QString& id, const QString& title) {
            FavoriteItem f;
            f.itemId = id; f.title = title; f.subtitle = QStringLiteral("An Artist");
            f.type = QStringLiteral("track"); f.thumbnailUrl = QStringLiteral("C:/covers/") + title + ".png";
            return f;
        };
        const FavoriteItem subFav     = trackRec(subHere,  QStringLiteral("Night Drive"));
        const FavoriteItem subGoneFav = trackRec(subGone,  QStringLiteral("Old Server Song"));
        const FavoriteItem goneFav    = trackRec(gonePath, QStringLiteral("Gone"));
        const FavoriteItem clipFav    = trackRec(clipUrl,  QStringLiteral("Live Opener"));
        const FavoriteItem jfFav      = trackRec(jfTrack,  QStringLiteral("Jelly Song"));
        const FavoriteItem ebsFav     = trackRec(ebsTrack, QStringLiteral("Shelf Song"));
        // AN ADD-ON'S favourites: a film, and an item of the add-on's own that happens to be TYPED "track" —
        // it names its add-on, so it is the add-on's to open and must not be taken by the track arm.
        auto addonRec = [](const QString& addon, const QString& id, const QString& type, const QString& title) {
            FavoriteItem f;
            f.addonId = addon; f.itemId = id; f.type = type; f.title = title;
            return f;
        };
        const FavoriteItem filmFav      = addonRec(QStringLiteral("com.example.films"), QStringLiteral("tt0816692"),
                                                   QStringLiteral("movie"), QStringLiteral("Interstellar"));
        const FavoriteItem radioFav     = addonRec(QStringLiteral("com.example.radio"), QStringLiteral("radio:1"),
                                                   QStringLiteral("track"), QStringLiteral("Radio Track"));
        const FavoriteItem lostFilmFav  = addonRec(QStringLiteral("com.example.gone"), QStringLiteral("tt0133093"),
                                                   QStringLiteral("movie"), QStringLiteral("The Matrix"));
        const FavoriteItem lostRadioFav = addonRec(QStringLiteral("com.example.gone-radio"), QStringLiteral("radio:9"),
                                                   QStringLiteral("track"), QStringLiteral("Lost Radio Track"));
        // The arms that were there before: a path-carrying local game, and the two native stores.
        FavoriteItem gameFav;
        gameFav.itemId = QStringLiteral("C:/roms/nes/a.nes"); gameFav.path = gameFav.itemId;
        gameFav.kind = QStringLiteral("game"); gameFav.type = QStringLiteral("game");
        gameFav.system = QStringLiteral("nes"); gameFav.title = QStringLiteral("A Game");
        gameFav.thumbnailUrl = QStringLiteral("C:/covers/a.png");
        FavoriteItem steamFav; steamFav.itemId = QStringLiteral("steam:1145360"); steamFav.type = QStringLiteral("game");
        steamFav.title = QStringLiteral("Hades");
        FavoriteItem epicFav;  epicFav.itemId = QStringLiteral("epic:Fortnite");  epicFav.type = QStringLiteral("game");
        epicFav.title = QStringLiteral("Fortnite");

        const QVector<FavoriteItem> all = { localFav, subFav, subGoneFav, goneFav, clipFav, jfFav, ebsFav,
                                            filmFav, radioFav, lostFilmFav, lostRadioFav, gameFav, steamFav, epicFav };
        for (const FavoriteItem& f : all) FavoritesStore::addFromSource(f);   // seeding, not starring: quiet
        CHECK(FavoritesStore::list().size() == all.size());

        // Route EVERY row the shelf would draw, with the love hook watching.
        struct Love { FavoriteItem f; bool loved; };
        QVector<Love> loves;
        FavoritesStore::setLoveHook([&loves](const FavoriteItem& f, bool loved) { loves.push_back({ f, loved }); });
        const QVector<FavoriteItem> before = FavoritesStore::list();
        QHash<QString, FavoriteRoute> routes;
        QHash<QString, MediaItem> rows;
        for (const FavoriteItem& f : before)
        {
            const MediaItem row = browse::favoriteShelfRow(f);
            rows.insert(f.itemId, row);
            routes.insert(f.itemId, browse::favoriteRouteFor(row, FavoritesStore::list(), world));
        }
        CHECK(routes.size() == all.size());

        // 8a. OPENING CHANGES NOTHING. No love, no un-love, and the store holds exactly what it held — same
        // records, same order, same timestamps. A route that re-added the favourite on the way in would send a
        // server star every time somebody pressed Play.
        CHECK(loves.isEmpty());
        const QVector<FavoriteItem> after = FavoritesStore::list();
        CHECK(after.size() == before.size());
        for (int i = 0; i < qMin(after.size(), before.size()); ++i)
            CHECK(after.at(i).itemId == before.at(i).itemId && after.at(i).ts == before.at(i).ts
                  && after.at(i).type == before.at(i).type && after.at(i).path == before.at(i).path);

        // 8b. THE SHELF ROW A TRACK BECOMES carries what the router reads: its id, type "track", and a "fav:"
        // marker naming NO add-on.
        {
            const MediaItem row = rows.value(localFav.itemId);
            CHECK(row.id == dawnPath);
            CHECK(row.type == QLatin1String("track"));
            CHECK(row.mime == QLatin1String("fav:"));
        }

        // 8c. A LOCAL TRACK OPENS BY ITS FILE, as kind "audio" — openRecent's file route, the one this track's
        // own Recents row re-opens by. The shelf row's title and cover travel with it.
        {
            const FavoriteRoute r = routes.value(localFav.itemId);
            const MediaItem row = rows.value(localFav.itemId);
            CHECK(r.how == FavoriteOpen::LocalTrack);
            CHECK(r.path == dawnPath);
            CHECK(r.kind == QLatin1String("audio"));
            CHECK(r.resumeKey == dawnPath);
            CHECK(r.title == row.title && r.thumb == row.thumbnailUrl);
            CHECK(asked.contains(dawnPath));                       // the file WAS asked about
        }

        // 8d. A SUBSONIC TRACK OPENS BY ITS QUALIFIED ID, through the qualified-TRACK door: openRecent consults
        // the resume key first, and a Track-kind id there is minted into a fresh stream url.
        {
            const FavoriteRoute r = routes.value(subFav.itemId);
            CHECK(r.how == FavoriteOpen::ServerTrack);
            CHECK(r.path == subHere && r.resumeKey == subHere);
            CHECK(r.kind == QLatin1String("audio"));
            CHECK(Subsonic::parse(r.resumeKey).kind == Subsonic::Kind::Track);
            CHECK(r.title == QLatin1String("Night Drive"));
            CHECK(!asked.contains(subHere));                       // never mistaken for a file
        }

        // 8e. A CUE TRACK is local, and opens by its clip url without being asked about as a file (it is not
        // one — the url names a span of one).
        {
            const FavoriteRoute r = routes.value(clipFav.itemId);
            CHECK(r.how == FavoriteOpen::LocalTrack);
            CHECK(r.path == clipUrl && r.kind == QLatin1String("audio"));
            CHECK(!asked.contains(clipUrl));
        }

        // 8f. A TRACK THAT CANNOT BE OPENED SAYS WHY, and it is its own sentence each time. None of them hands
        // openRecent anything, and none of them is the add-on message.
        CHECK(routes.value(goneFav.itemId).how == FavoriteOpen::TrackFileGone);
        CHECK(routes.value(subGoneFav.itemId).how == FavoriteOpen::TrackServerGone);
        CHECK(routes.value(jfFav.itemId).how == FavoriteOpen::TrackNoDoor);
        CHECK(routes.value(ebsFav.itemId).how == FavoriteOpen::TrackNoDoor);
        CHECK(!asked.contains(jfTrack) && !asked.contains(ebsTrack) && !asked.contains(subGone));
        for (const FavoriteItem& f : { goneFav, subGoneFav, jfFav, ebsFav })
        {
            const FavoriteRoute r = routes.value(f.itemId);
            CHECK(r.path.isEmpty() && r.kind.isEmpty() && r.resumeKey.isEmpty());
        }
        const QString title = QStringLiteral("Dawn");
        const QString sFile = browse::favoriteOpenSentence(FavoriteOpen::TrackFileGone, title);
        const QString sSrv  = browse::favoriteOpenSentence(FavoriteOpen::TrackServerGone, title);
        const QString sDoor = browse::favoriteOpenSentence(FavoriteOpen::TrackNoDoor, title);
        for (const QString& s : { sFile, sSrv, sDoor })
        {
            CHECK(!s.isEmpty());
            CHECK(s.contains(title));                              // it names the track
            CHECK(!s.contains(QLatin1String("addon"), Qt::CaseInsensitive));
            CHECK(!s.contains(QLatin1String("add-on"), Qt::CaseInsensitive));
        }
        CHECK(sFile != sSrv && sFile != sDoor && sSrv != sDoor);
        CHECK(sFile.contains(QLatin1String("moved or deleted")));
        CHECK(sSrv.contains(QLatin1String("music server")));
        for (FavoriteOpen quiet : { FavoriteOpen::ReopenByPath, FavoriteOpen::NativeStore, FavoriteOpen::LocalTrack,
                                    FavoriteOpen::ServerTrack, FavoriteOpen::Addon, FavoriteOpen::AddonMissing })
            CHECK(browse::favoriteOpenSentence(quiet, title).isEmpty());

        // 8g. AN ADD-ON'S FAVOURITE STILL ROUTES TO ITS ADD-ON, and still reports it missing when it has gone —
        // INCLUDING the ones typed "track", which name their add-on and so are not the track arm's.
        {
            const FavoriteRoute film = routes.value(filmFav.itemId);
            CHECK(film.how == FavoriteOpen::Addon && film.addonId == QLatin1String("com.example.films"));
            const FavoriteRoute radio = routes.value(radioFav.itemId);
            CHECK(radio.how == FavoriteOpen::Addon && radio.addonId == QLatin1String("com.example.radio"));
            const FavoriteRoute lostFilm = routes.value(lostFilmFav.itemId);
            CHECK(lostFilm.how == FavoriteOpen::AddonMissing && lostFilm.addonId == QLatin1String("com.example.gone"));
            const FavoriteRoute lostRadio = routes.value(lostRadioFav.itemId);
            CHECK(lostRadio.how == FavoriteOpen::AddonMissing
                  && lostRadio.addonId == QLatin1String("com.example.gone-radio"));
            for (const FavoriteRoute& r : { film, radio, lostFilm, lostRadio }) CHECK(r.path.isEmpty());
        }

        // 8h. THE ARMS THAT WERE THERE BEFORE ARE UNTOUCHED. A path-carrying record re-opens by the STORED
        // record's path, kind, id, title and cover; Steam and Epic ids go to their stores.
        {
            const FavoriteRoute g = routes.value(gameFav.itemId);
            CHECK(g.how == FavoriteOpen::ReopenByPath);
            CHECK(g.path == gameFav.path && g.kind == QLatin1String("game") && g.resumeKey == gameFav.itemId);
            CHECK(g.title == gameFav.title && g.thumb == gameFav.thumbnailUrl);
            CHECK(routes.value(steamFav.itemId).how == FavoriteOpen::NativeStore);
            CHECK(routes.value(epicFav.itemId).how == FavoriteOpen::NativeStore);
            // The track arm sits AFTER the path arm: a record that carries a path re-opens by it whatever its
            // type — the order openFavorite always had.
            FavoriteItem pathTrack = trackRec(QStringLiteral("C:/music/x.flac"), QStringLiteral("X"));
            pathTrack.path = pathTrack.itemId; pathTrack.kind = QStringLiteral("audio");
            const FavoriteRoute pt = browse::favoriteRouteFor(browse::favoriteShelfRow(pathTrack), { pathTrack }, world);
            CHECK(pt.how == FavoriteOpen::ReopenByPath && pt.path == pathTrack.path);
        }

        // 8i. AN UNANSWERED WORLD answers false: nothing opens that nobody said was there.
        {
            const browse::FavoriteWorld blind;
            CHECK(browse::favoriteRouteFor(browse::favoriteShelfRow(localFav), {}, blind).how
                  == FavoriteOpen::TrackFileGone);
            CHECK(browse::favoriteRouteFor(browse::favoriteShelfRow(subFav), {}, blind).how
                  == FavoriteOpen::TrackServerGone);
            CHECK(browse::favoriteRouteFor(browse::favoriteShelfRow(filmFav), {}, blind).how
                  == FavoriteOpen::AddonMissing);
        }

        FavoritesStore::setLoveHook({});
        for (const FavoriteItem& f : all) FavoritesStore::remove(f.itemId);
        CHECK(FavoritesStore::list().isEmpty());
    }

    // ---- §9 ADD TO PLAYLIST AND DOWNLOAD ON A CLASSIC TRACK ROW (issue #365) ------------------------------
    // #297 gave a track row Favorite in both classic menus and left two verbs the themed chooser has: Add to
    // playlist (reachable only through P / the pad's R) and Download (not at all). Download was driven on the
    // themed layout before being offered here, and it only ever did anything on a remote add-on's track — so
    // this pins the rows, and pins that Download is NOT spread to the kinds where it can only say "Nothing here
    // could be downloaded."
    {
        using browse::TrackAddon;
        using browse::TrackMenuVerbs;
        const TrackAddon kAll[] = { TrackAddon::None, TrackAddon::Script, TrackAddon::Remote };
        const QString dawnPath = QStringLiteral("C:/music/Vol 1/01 Dawn.flac");

        // 9a. EVERY LOCAL TRACK ROW OF A REAL ALBUM: Add to playlist, and no Download — in ANY add-on context. A
        // library level has no add-on, but the answer must not depend on that: a local file is already local.
        // The album's own action row ("Play album", a '_' row) gets neither.
        const MusicLibrary::Index idx = oneAlbumIndex();
        const MediaCatalog album = browse::musicAlbumCatalog(idx, QString::fromLatin1(kAlbumKey), noCover);
        int localTracks = 0, others = 0;
        MediaItem dawn;
        for (const MediaItem& it : album.items)
        {
            const bool isTrack = browse::queueTargetFor(it).what == browse::QueueAdd::Track;
            for (TrackAddon a : kAll)
            {
                const TrackMenuVerbs v = browse::trackMenuVerbsFor(it, a);
                CHECK(v.playlist == isTrack);    // the queue's reading of "a track" and this one agree, row for row
                CHECK(!v.download);
                CHECK(v.any() == isTrack);
            }
            if (isTrack) ++localTracks; else ++others;
            if (it.url == dawnPath) dawn = it;
        }
        CHECK(localTracks == 2);
        CHECK(others >= 1);                      // the Play-album row really was asked
        CHECK(dawn.url == dawnPath);

        // 9b. A SUBSONIC TRACK ROW, from the SAME builder over an index whose ids are Subsonic's own — the shape
        // SubsonicClient's index has (IndexTrack::path is the qualified id, never a signed url). Add to playlist,
        // and no Download: #365 pressed the themed Download on exactly this row and no request reached the server.
        {
            const QString srv = QStringLiteral("3f2b8c1e-6a4d-4e0b-9a51-2c7d8e9f0a1b");
            MusicLibrary::Index sub = oneAlbumIndex();
            MusicLibrary::Album& b = sub.artists[0].albums[0];
            b.key = Subsonic::qualify(srv, Subsonic::Kind::Album, QStringLiteral("al-1"));
            b.tracks[0].path = Subsonic::qualify(srv, Subsonic::Kind::Track, QStringLiteral("tr-1"));
            b.tracks[1].path = Subsonic::qualify(srv, Subsonic::Kind::Track, QStringLiteral("tr-2"));
            CHECK(Subsonic::isQualified(b.key) && Subsonic::isQualified(b.tracks[0].path));   // fixture sanity
            const MediaCatalog subAlbum = browse::musicAlbumCatalog(sub, b.key, noCover);
            int subTracks = 0;
            for (const MediaItem& it : subAlbum.items)
            {
                if (!Subsonic::isQualified(it.url)) continue;
                ++subTracks;
                CHECK(browse::queueTargetFor(it).what == browse::QueueAdd::Track);
                for (TrackAddon a : kAll)
                {
                    const TrackMenuVerbs v = browse::trackMenuVerbsFor(it, a);
                    CHECK(v.playlist);
                    CHECK(!v.download);
                }
            }
            CHECK(subTracks == 2);   // a builder that emitted nothing would pass the loop above vacuously
        }

        // 9c. AN ADD-ON'S TRACK, typed any of the three music-leaf spellings an add-on uses. With no add-on behind
        // it, nothing (it is not an add-on's row then, and not a library row either). Under a SCRIPT add-on — the
        // AIO catalog's MusicBrainz track shape, metadata with no url — Add to playlist but no Download: the crawl
        // has no arm for it and the themed press said "Nothing here could be downloaded." Under a REMOTE add-on,
        // both: its /stream is what the crawl downloads, and the themed press did download it.
        for (const char* type : { "track", "song", "music" })
        {
            MediaItem t;
            t.id = QStringLiteral("fx-song-1"); t.title = QStringLiteral("Addon Song One");
            t.type = QString::fromLatin1(type); t.expandable = false;
            const TrackMenuVerbs none = browse::trackMenuVerbsFor(t, TrackAddon::None);
            CHECK(!none.playlist && !none.download);
            const TrackMenuVerbs script = browse::trackMenuVerbsFor(t, TrackAddon::Script);
            CHECK(script.playlist && !script.download);
            const TrackMenuVerbs remote = browse::trackMenuVerbsFor(t, TrackAddon::Remote);
            CHECK(remote.playlist && remote.download);
        }

        // 9d. NOTHING THAT IS NOT A TRACK ROW. Without this, 9a-9c are satisfied by a function that offers both
        // verbs on everything under a remote add-on — and the Start menu would grow Download on a film, which is
        // not this menu's verb to carry.
        {
            auto row = [](const QString& type, bool expandable, const QString& id) {
                MediaItem m; m.type = type; m.expandable = expandable; m.id = id; m.title = QStringLiteral("x");
                return m;
            };
            const MediaItem film      = row(QStringLiteral("movie"), false, QStringLiteral("tt0816692"));
            const MediaItem episode   = row(QStringLiteral("episode"), false, QStringLiteral("tt1:1:1"));
            const MediaItem book      = row(QStringLiteral("audiobook"), false, QStringLiteral("ab-1"));
            const MediaItem albumRow  = row(QStringLiteral("music"), true, QStringLiteral("al-1"));   // a container
            const MediaItem artist    = row(QStringLiteral("track"), true, QStringLiteral("ar-1"));   // expandable
            const MediaItem synthetic = row(QStringLiteral("_playlists"), false, QStringLiteral("_playlists"));
            const MediaItem guidance  = row(QStringLiteral("info"), false, QStringLiteral("info:empty"));
            const MediaItem divider   = row(QStringLiteral("rechdr"), false, QStringLiteral("hdr"));
            const MediaItem noId      = row(QStringLiteral("track"), false, QString());   // unfileable
            // A row carrying the LIBRARY track mime that names no file: a library row, never an add-on's, so an
            // add-on context must not rescue it into a Download (§7's noFile row, asked here in every context).
            MediaItem noFile; noFile.type = QString::fromLatin1(browse::kMusicTrackType);
            noFile.mime = QString::fromLatin1(browse::kMusicTrackPrefix) + QString::fromLatin1(kAlbumKey);
            noFile.id = QStringLiteral("C:/music/gone.flac");
            for (const MediaItem& it : { film, episode, book, albumRow, artist, synthetic, guidance, divider, noId,
                                         noFile })
                for (TrackAddon a : kAll)
                {
                    const TrackMenuVerbs v = browse::trackMenuVerbsFor(it, a);
                    CHECK(!v.playlist && !v.download && !v.any());
                }
            // ...and the album-list rows of a real artist (the album row, Play all, Shuffle all).
            const MediaCatalog artistCat = browse::musicArtistCatalog(idx, QStringLiteral("the hollows"), noCover);
            CHECK(!artistCat.items.isEmpty());
            for (const MediaItem& it : artistCat.items)
                for (TrackAddon a : kAll)
                    CHECK(!browse::trackMenuVerbsFor(it, a).any());
        }

        // 9e. THE PLAYLIST VERB'S SHAPE — the P key's, lifted into browse::queueOnRowCopy. The picker it opens is a
        // nested loop, so it must not run inside the press; and addItemToPlaylistInteractive reads its item AFTER
        // those loops, so what it gets must be a copy taken at the press, not a view into the list the row came
        // from. The list is overwritten at the SAME index (a re-present filing a different row there) and then
        // reallocated, both after the call and before the turn: a reference would see the other row, or freed
        // memory holding it.
        {
            QVector<MediaItem> rows = album.items;
            int at = -1;
            for (int i = 0; i < rows.size(); ++i)
                if (rows[i].url == dawnPath) at = i;
            CHECK(at >= 0);
            if (at >= 0)
            {
                const MediaItem pressed = rows[at];
                QObject owner;
                QVector<MediaItem> seen;
                browse::queueOnRowCopy(&owner, rows[at], [&seen](const MediaItem& m) { seen.push_back(m); });
                CHECK(seen.isEmpty());                                      // QUEUED: nothing ran in the press
                rows[at].title = QStringLiteral("Somebody Else");
                rows[at].id = rows[at].url = QStringLiteral("C:/music/elsewhere.flac");
                rows[at].type = QStringLiteral("movie");
                rows.resize(rows.size() + 256);                             // ...and the list reallocates
                QCoreApplication::sendPostedEvents(nullptr, 0);
                CHECK(seen.size() == 1);                                    // once, a turn later
                if (seen.size() == 1)
                {
                    CHECK(seen.at(0).id == pressed.id);
                    CHECK(seen.at(0).url == pressed.url);
                    CHECK(seen.at(0).title == pressed.title);
                    CHECK(seen.at(0).type == pressed.type);
                    CHECK(seen.at(0).mime == pressed.mime);
                    CHECK(seen.at(0).id == dawnPath);
                }
                QCoreApplication::sendPostedEvents(nullptr, 0);
                CHECK(seen.size() == 1);                                    // and never again
            }
            // The owner going first cancels it: a menu's HomeView torn down before the turn must not be called into.
            bool ran = false;
            {
                QObject gone;
                browse::queueOnRowCopy(&gone, dawn, [&ran](const MediaItem&) { ran = true; });
            }
            QCoreApplication::sendPostedEvents(nullptr, 0);
            CHECK(!ran);
        }
    }

    if (g_fails) { std::printf("LEAFROUTE: %d failure(s)\n", g_fails); return 1; }
    std::printf("LEAFROUTE-OK\n");
    return 0;
}
