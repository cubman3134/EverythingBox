// Headless check of MULTI-ALBUM music queues (src/core/MusicQueue + the two action rows that reach them in
// src/browse/MusicCatalogs): "Play all" and "Shuffle all" for an artist, and "Shuffle all music" for the
// whole library.
//
// WHY THIS EXISTS. Until these verbs did, every queue this app could build was ONE ALBUM — openMusicAlbum
// queues an album, openAudioPath queues a folder — and the only producer that could span records was the
// multi-select file dialog, which the themed surface does not expose. That made two shipped features nearly
// unreachable: crossfade (#141) suppresses inside a record on purpose, so on a tidy library it never fired;
// and ReplayGain's track mode exists for shuffled listening that could not happen. So the last section here
// is not decoration — it runs the REAL Crossfade decision over a REAL shuffled queue and pins that the
// boundaries it takes are the ones between records and the ones it refuses are the ones inside a record.
//
// THE FIXTURES ARE REAL TAGGED FILES, written from tools/MusicFixtures.h and SCANNED by the real
// MusicLibrary code, for the reason probe_musicbrowse gives: a builder tested against a hand-built index
// proves only that the two agree. The ONE hand-built index here is the 20,000-track one in the performance
// section, which is measuring a memory walk and would otherwise be measuring twenty thousand file writes.
//
// What it pins:
//   1. PLAY ALL FOR AN ARTIST is album (year, then title) then disc then track order, across every album —
//      not one album, not the albums in folder order, and not the multi-disc set flattened by filename.
//   2. SHUFFLE ALL FOR AN ARTIST / FOR THE LIBRARY contains every track EXACTLY ONCE...
//   3. ...and is a shuffle of the WHOLE SET, not a shuffle of each album stitched back together. Pinned as
//      "the shuffled queue has more album RUNS than the library has albums", which is the one property that
//      tells the two apart: a per-album shuffle keeps every record contiguous and so never crosses one.
//   4. DETERMINISM in the seed, and that shuffling actually moves something.
//   5. STALE / EMPTY ROUTES: an unknown artist key is an empty queue (never "some other artist"), an empty
//      library builds nothing, and a one-track queue shuffles to itself rather than misbehaving.
//   6. THE ROWS that reach all of this: their type, their key round-trip through musicKeyOf (including an
//      artist key containing a ':'), their counts agreeing with the queue they build, and the two places
//      they must NOT appear — a one-track artist and a one-track library have nothing to shuffle.
//   7. CROSSFADE INTERPLAY over a real shuffled queue (see above).
//   8. THE QUEUE BUILD IS NOT A STALL: 20,000 tracks flattened and shuffled inside one frame, which is the
//      claim MusicQueue.h makes when it says this belongs on the GUI thread.
//
// Prints MUSICQUEUE-OK on success; any failure prints MUSICQUEUE-FAIL <cond> (line) and exits non-zero.
//
// Isolation: AppPaths::dataDir() is this process's own scratch dir (issue #42), so the fixture library is
// written under it and goes away at exit.
#include "MusicQueue.h"
#include "MusicCatalogs.h"
#include "LeafRoute.h"   // browse::queueTargetFor — the row -> "add this to the queue" claim (#193 inc 2)
#include "MusicLibrary.h"
#include "AudioTags.h"
#include "Crossfade.h"
#include "AppPaths.h"
#include "MusicFixtures.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QSet>
#include <QString>
#include <QStringList>
#include <algorithm>
#include <cstdio>

static int g_fails = 0;

#define CHECK(cond)                                                                        \
    do {                                                                                   \
        if (!(cond)) { std::printf("MUSICQUEUE-FAIL %s (%d)\n", #cond, __LINE__); ++g_fails; } \
    } while (0)

using MusicLibrary::Album;
using MusicLibrary::Artist;
using MusicLibrary::Index;
using MusicQueue::Entry;

// ---------------------------------------------------------------------------------------------------------
// Fixture writer over MusicFixtures.h's byte builders (probe_musicbrowse's, minus the artwork arguments this
// probe has no use for). An empty QString means "write no frame at all".
// ---------------------------------------------------------------------------------------------------------
static bool writeMp3(const QString& path, const QString& title, const QString& artist,
                     const QString& albumArtist, const QString& album,
                     const QString& trck = QString(), const QString& tpos = QString(),
                     const QString& year = QString())
{
    QByteArray frames;
    if (!title.isEmpty())       frames.append(id3TextFrame("TIT2", title));
    if (!artist.isEmpty())      frames.append(id3TextFrame("TPE1", artist));
    if (!albumArtist.isEmpty()) frames.append(id3TextFrame("TPE2", albumArtist));
    if (!album.isEmpty())       frames.append(id3TextFrame("TALB", album));
    if (!trck.isEmpty())        frames.append(id3TextFrame("TRCK", trck));
    if (!tpos.isEmpty())        frames.append(id3TextFrame("TPOS", tpos));
    if (!year.isEmpty())        frames.append(id3TextFrame("TDRC", year));
    return writeFixture(path, mp3File(frames));
}

static QStringList titlesOf(const QVector<Entry>& q)
{
    QStringList out;
    for (const Entry& e : q) out << e.title;
    return out;
}

static QStringList pathsOf(const QVector<Entry>& q)
{
    QStringList out;
    for (const Entry& e : q) out << e.path;
    return out;
}

// How many CONTIGUOUS RUNS of one album the queue falls into. An ordered queue (or a per-album shuffle
// stitched back together) has exactly one run per album; a shuffle of the whole set breaks the records apart
// and has many more. This single number is what tells the two implementations apart.
static int albumRuns(const QVector<Entry>& q)
{
    int runs = 0;
    for (int i = 0; i < q.size(); ++i)
        if (i == 0 || q[i].albumKey != q[i - 1].albumKey) ++runs;
    return runs;
}

// The index's artist key for a display name (the probe never hard-codes a key: it is a case-folding rule
// that belongs to MusicLibrary, and re-spelling it here would be a second copy of it).
static QString keyForArtist(const Index& idx, const QString& displayName)
{
    for (const Artist& a : idx.artists) if (MusicLibrary::displayArtist(a) == displayName) return a.key;
    return QString();
}

static MediaItem at(const MediaCatalog& c, int i)
{
    return (i >= 0 && i < c.items.size()) ? c.items[i] : MediaItem{};
}

// One side of a crossfade boundary, gathered exactly the way MainWindow::crossfadeTrackFacts gathers it:
// the album tag and the length from AudioTags (the same reader the library is built on), the folder for the
// untagged fallback. Copied in shape rather than shared because that function is a static inside
// MainWindow.cpp, which this probe cannot link; what matters is that the RULES come from Crossfade.h, which
// it does link, and which is where every decision actually lives.
static Crossfade::Track facts(const QString& path)
{
    Crossfade::Track t;
    const QFileInfo fi(path);
    t.folder = fi.absolutePath();
    const AudioTags::Tags tags = AudioTags::read(path);
    t.album = tags.album;
    t.durationSec = double(tags.durationSec);
    return t;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    const QString base = AppPaths::dataDir() + QStringLiteral("/musicqueue");
    const QString root = base + QStringLiteral("/root");
    QDir().mkpath(root);

    // --- The fixture library ---------------------------------------------------------------------------
    // Aphex Twin: THREE albums, deliberately written in an order (and in folders) whose alphabetical walk is
    // not their release order, so a builder that walked the filesystem would fail the ordering assertion.
    const QString dirA1 = root + QStringLiteral("/Aphex Twin/Ambient Works");
    CHECK(writeMp3(dirA1 + QStringLiteral("/01.mp3"), QStringLiteral("Xtal"), QStringLiteral("Aphex Twin"),
                   QStringLiteral("Aphex Twin"), QStringLiteral("Ambient Works"),
                   QStringLiteral("1/2"), QString(), QStringLiteral("1992")));
    CHECK(writeMp3(dirA1 + QStringLiteral("/02.mp3"), QStringLiteral("Tha"), QStringLiteral("Aphex Twin"),
                   QStringLiteral("Aphex Twin"), QStringLiteral("Ambient Works"),
                   QStringLiteral("2/2"), QString(), QStringLiteral("1992")));

    const QString dirA2 = root + QStringLiteral("/Aphex Twin/Richard D James");
    CHECK(writeMp3(dirA2 + QStringLiteral("/01.mp3"), QStringLiteral("4"), QStringLiteral("Aphex Twin"),
                   QStringLiteral("Aphex Twin"), QStringLiteral("Richard D James"),
                   QStringLiteral("1/3"), QString(), QStringLiteral("1995")));
    CHECK(writeMp3(dirA2 + QStringLiteral("/02.mp3"), QStringLiteral("Cornish Acid"),
                   QStringLiteral("Aphex Twin"), QStringLiteral("Aphex Twin"),
                   QStringLiteral("Richard D James"), QStringLiteral("2/3"), QString(),
                   QStringLiteral("1995")));
    CHECK(writeMp3(dirA2 + QStringLiteral("/03.mp3"), QStringLiteral("Peek"), QStringLiteral("Aphex Twin"),
                   QStringLiteral("Aphex Twin"), QStringLiteral("Richard D James"),
                   QStringLiteral("3/3"), QString(), QStringLiteral("1995")));

    // ...and one of them is a MULTI-DISC set, one folder per disc, with filenames chosen so path order
    // (Disc 1/z, Disc 1/a, Disc 2/a) is not disc-then-track order.
    const QString dirA3 = root + QStringLiteral("/Aphex Twin/Drukqs");
    CHECK(writeMp3(dirA3 + QStringLiteral("/Disc 1/z.mp3"), QStringLiteral("Jynweythek"),
                   QStringLiteral("Aphex Twin"), QStringLiteral("Aphex Twin"), QStringLiteral("Drukqs"),
                   QStringLiteral("1/2"), QStringLiteral("1/2"), QStringLiteral("2001")));
    CHECK(writeMp3(dirA3 + QStringLiteral("/Disc 1/a.mp3"), QStringLiteral("Vordhosbn"),
                   QStringLiteral("Aphex Twin"), QStringLiteral("Aphex Twin"), QStringLiteral("Drukqs"),
                   QStringLiteral("2/2"), QStringLiteral("1/2"), QStringLiteral("2001")));
    CHECK(writeMp3(dirA3 + QStringLiteral("/Disc 2/a.mp3"), QStringLiteral("Avril 14th"),
                   QStringLiteral("Aphex Twin"), QStringLiteral("Aphex Twin"), QStringLiteral("Drukqs"),
                   QStringLiteral("1/1"), QStringLiteral("2/2"), QStringLiteral("2001")));

    // A second ordinary artist, so the LIBRARY queue has an artist boundary in it.
    const QString dirB = root + QStringLiteral("/Boards of Canada/Geogaddi");
    CHECK(writeMp3(dirB + QStringLiteral("/01.mp3"), QStringLiteral("Sixtyten"),
                   QStringLiteral("Boards of Canada"), QStringLiteral("Boards of Canada"),
                   QStringLiteral("Geogaddi"), QStringLiteral("1/2"), QString(), QStringLiteral("1998")));
    CHECK(writeMp3(dirB + QStringLiteral("/02.mp3"), QStringLiteral("Dawn Chorus"),
                   QStringLiteral("Boards of Canada"), QStringLiteral("Boards of Canada"),
                   QStringLiteral("Geogaddi"), QStringLiteral("2/2"), QString(), QStringLiteral("1998")));

    // A ONE-TRACK artist: nothing to order and nothing to shuffle, so the two action rows must not appear.
    const QString dirC = root + QStringLiteral("/Plaid/Not For Threes");
    CHECK(writeMp3(dirC + QStringLiteral("/01.mp3"), QStringLiteral("Prague Radio"), QStringLiteral("Plaid"),
                   QStringLiteral("Plaid"), QStringLiteral("Not For Threes"), QStringLiteral("1/1"),
                   QString(), QStringLiteral("1997")));

    // An artist whose name contains a ':' — the route key is arbitrary tag text, and a colon-splitting
    // reader would truncate it and play the wrong artist (or none).
    const QString dirD = root + QStringLiteral("/Prefuse 73/One Word");
    CHECK(writeMp3(dirD + QStringLiteral("/01.mp3"), QStringLiteral("Plastic"),
                   QStringLiteral("Prefuse: 73"), QStringLiteral("Prefuse: 73"),
                   QStringLiteral("One Word"), QStringLiteral("1/2"), QString(), QStringLiteral("2003")));
    CHECK(writeMp3(dirD + QStringLiteral("/02.mp3"), QStringLiteral("Detchibe"),
                   QStringLiteral("Prefuse: 73"), QStringLiteral("Prefuse: 73"),
                   QStringLiteral("One Word"), QStringLiteral("2/2"), QString(), QStringLiteral("2003")));

    // The COMPILATION: one album artist, a different track artist on every track. Its tracks must stay ONE
    // record for the crossfade rule even though nothing else about them agrees.
    const QString dirE = root + QStringLiteral("/Compilations/Warp Sampler");
    CHECK(writeMp3(dirE + QStringLiteral("/a.mp3"), QStringLiteral("Windowlicker"),
                   QStringLiteral("Aphex Twin"), QStringLiteral("Various Artists"),
                   QStringLiteral("Warp Sampler"), QStringLiteral("1/3"), QString(), QStringLiteral("1999")));
    CHECK(writeMp3(dirE + QStringLiteral("/b.mp3"), QStringLiteral("Roygbiv"),
                   QStringLiteral("Boards of Canada"), QStringLiteral("Various Artists"),
                   QStringLiteral("Warp Sampler"), QStringLiteral("2/3"), QString(), QStringLiteral("1999")));
    CHECK(writeMp3(dirE + QStringLiteral("/c.mp3"), QStringLiteral("Eyen"), QStringLiteral("Plaid"),
                   QStringLiteral("Various Artists"), QStringLiteral("Warp Sampler"),
                   QStringLiteral("3/3"), QString(), QStringLiteral("1999")));

    const Index idx = MusicLibrary::buildIndex(MusicLibrary::scanFolder(root));
    CHECK(idx.trackCount == 16);
    CHECK(idx.albumCount == 7);
    CHECK(idx.artists.size() == 5);

    const QString aphex   = keyForArtist(idx, QStringLiteral("Aphex Twin"));
    const QString plaid   = keyForArtist(idx, QStringLiteral("Plaid"));
    const QString prefuse = keyForArtist(idx, QStringLiteral("Prefuse: 73"));
    CHECK(!aphex.isEmpty());
    CHECK(!plaid.isEmpty());
    CHECK(prefuse.contains(QLatin1Char(':')));   // the key really does carry the colon it must survive

    // --- 1. Play all for an artist: EVERY album, in year-then-title, disc, track order -------------------
    {
        const QVector<Entry> q = MusicQueue::forArtist(idx, aphex);
        CHECK(q.size() == 8);
        // Written out in full: this is the ONE assertion that says "an artist play-all spans albums", and a
        // count alone would pass for a queue that played the first album three times.
        const QStringList want = { QStringLiteral("Xtal"),        QStringLiteral("Tha"),
                                   QStringLiteral("4"),           QStringLiteral("Cornish Acid"),
                                   QStringLiteral("Peek"),        QStringLiteral("Jynweythek"),
                                   QStringLiteral("Vordhosbn"),   QStringLiteral("Avril 14th") };
        CHECK(titlesOf(q) == want);
        // Three albums, each in ONE contiguous run: an ordered discography, not an interleaved one.
        CHECK(albumRuns(q) == 3);
        // Every entry names the artist and the record it came from — what the now-playing page reads.
        for (const Entry& e : q)
        {
            CHECK(e.artist == QStringLiteral("Aphex Twin"));
            CHECK(!e.albumKey.isEmpty());
            CHECK(!e.path.isEmpty());
        }
        // The compilation track by Aphex Twin is NOT in here: it belongs to Various Artists, which is what
        // album-artist grouping is for (#74's headline bug, restated as a queue).
        CHECK(!titlesOf(q).contains(QStringLiteral("Windowlicker")));
    }

    // --- 2. The whole library, in index order -----------------------------------------------------------
    {
        const QVector<Entry> q = MusicQueue::forLibrary(idx);
        CHECK(q.size() == idx.trackCount);
        const QStringList want = { // Aphex Twin
                                   QStringLiteral("Xtal"), QStringLiteral("Tha"), QStringLiteral("4"),
                                   QStringLiteral("Cornish Acid"), QStringLiteral("Peek"),
                                   QStringLiteral("Jynweythek"), QStringLiteral("Vordhosbn"),
                                   QStringLiteral("Avril 14th"),
                                   // Boards of Canada
                                   QStringLiteral("Sixtyten"), QStringLiteral("Dawn Chorus"),
                                   // Plaid
                                   QStringLiteral("Prague Radio"),
                                   // Prefuse: 73
                                   QStringLiteral("Plastic"), QStringLiteral("Detchibe"),
                                   // Various Artists
                                   QStringLiteral("Windowlicker"), QStringLiteral("Roygbiv"),
                                   QStringLiteral("Eyen") };
        CHECK(titlesOf(q) == want);
        CHECK(albumRuns(q) == idx.albumCount);   // ordered: exactly one run per record
        // A compilation track carries its OWN artist, not the album's — the queue title has to be able to
        // say "Windowlicker — Aphex Twin" on a library-wide shuffle.
        CHECK(q.at(13).artist == QStringLiteral("Aphex Twin"));
        CHECK(q.at(15).artist == QStringLiteral("Plaid"));
    }

    // --- 3. Shuffle: every track exactly once, and the RECORDS BROKEN APART -----------------------------
    {
        const QVector<Entry> base = MusicQueue::forLibrary(idx);
        QStringList basePaths = pathsOf(base);
        basePaths.sort();

        for (quint32 seed : { 1u, 2u, 3u, 4u, 5u })
        {
            QVector<Entry> q = base;
            MusicQueue::shuffle(q, seed);
            // Completeness: the same multiset of files, nothing dropped and nothing duplicated. (A shuffle
            // that assigns instead of swapping duplicates one entry and loses another, and a count check
            // alone would not notice.)
            CHECK(q.size() == base.size());
            QStringList got = pathsOf(q);
            got.sort();
            CHECK(got == basePaths);
            CHECK(QSet<QString>(got.begin(), got.end()).size() == base.size());
            // THE property that separates a whole-set shuffle from per-album shuffling stitched together:
            // a per-album shuffle leaves every record contiguous, so its run count is exactly the album
            // count. A real shuffle of 16 tracks over 7 records cannot plausibly land there.
            CHECK(albumRuns(q) > idx.albumCount);
            // ...and it actually moved something.
            CHECK(pathsOf(q) != pathsOf(base));
        }

        // Deterministic in the seed, and different seeds give different orders (16! makes a collision a
        // non-event, so this is a real assertion rather than a flake).
        QVector<Entry> a = base, b = base, c = base;
        MusicQueue::shuffle(a, 99u);
        MusicQueue::shuffle(b, 99u);
        MusicQueue::shuffle(c, 100u);
        CHECK(pathsOf(a) == pathsOf(b));
        CHECK(pathsOf(a) != pathsOf(c));

        // An artist shuffle is the same shuffle over the smaller set: still complete, still crossing records.
        QVector<Entry> art = MusicQueue::forArtist(idx, aphex);
        QStringList artBase = pathsOf(art);
        artBase.sort();
        MusicQueue::shuffle(art, 7u);
        QStringList artGot = pathsOf(art);
        artGot.sort();
        CHECK(artGot == artBase);
        CHECK(albumRuns(art) > 3);
    }

    // --- 3b. ONE RECORD (MusicQueue::forAlbum, issue #193 increment 2) -----------------------------------
    // "Add this album to the queue" needs the record's tracks in the record's own order, and it needs them
    // from the SAME walk the artist and library queues use — a second loop over Album::tracks at the call
    // site would be a second definition of album order, and the two would drift the first time either
    // changed. Pinned as an identity against forArtist rather than against a retyped list: what forAlbum
    // returns for one record must be exactly the slice of that artist's discography belonging to it.
    {
        const QVector<Entry> disco = MusicQueue::forArtist(idx, aphex);
        // Take the first record's key off the discography — a slice, not a hand-typed key, so this stays
        // true if the fixtures change.
        const QString firstKey = disco.first().albumKey;
        QVector<Entry> slice;
        for (const Entry& e : disco) if (e.albumKey == firstKey) slice.push_back(e);
        CHECK(slice.size() > 1);                       // the record really has several tracks to order

        const QVector<Entry> one = MusicQueue::forAlbum(idx, firstKey);
        CHECK(one.size() == slice.size());
        CHECK(titlesOf(one) == titlesOf(slice));       // same tracks, same order
        CHECK(albumRuns(one) == 1);                    // exactly one record, contiguous
        for (int i = 0; i < one.size(); ++i)
        {
            CHECK(one.at(i).path == slice.at(i).path);
            CHECK(one.at(i).albumKey == firstKey);
            CHECK(one.at(i).artist == slice.at(i).artist);   // the album-artist fallback, not re-derived
        }
        // The whole library's copy of that record agrees too — three walks, one order.
        QVector<Entry> fromLib;
        for (const Entry& e : MusicQueue::forLibrary(idx)) if (e.albumKey == firstKey) fromLib.push_back(e);
        CHECK(titlesOf(fromLib) == titlesOf(one));

        // A stale route: the row named a record the rescan dropped. Empty, never somebody else's album.
        CHECK(MusicQueue::forAlbum(idx, QStringLiteral("no such record")).isEmpty());
        CHECK(MusicQueue::forAlbum(idx, QString()).isEmpty());
        CHECK(MusicQueue::forAlbum(Index{}, firstKey).isEmpty());

        // ---- THE CHAIN, end to end: a browse row -> browse::queueTargetFor -> tracks to add ----
        // This is the claim that the reach verbs are not a silent no-op. Every row of a real album level is
        // claimed as a Track or a Record, and what the claim names really is findable in the index: a
        // record's key builds a non-empty queue, and a track's path is one of that queue's entries. A track
        // whose path is NOT in the album's queue is exactly the failure that would toast "no longer in your
        // library" over a track sitting on screen.
        const browse::MusicCoverFn noCover = [](const Album&) { return QString(); };
        const MediaCatalog level = browse::musicAlbumCatalog(idx, firstKey, noCover);
        CHECK(level.items.size() == slice.size() + 1);   // the "Play album" row plus each track
        int addTracks = 0, addAlbums = 0;
        for (const MediaItem& row : level.items)
        {
            const browse::QueueTarget t = browse::queueTargetFor(row);
            CHECK(t.ok());
            const QVector<Entry> built = MusicQueue::forAlbum(idx, t.albumKey);
            CHECK(!built.isEmpty());
            if (t.what == browse::QueueAdd::Track)
            {
                ++addTracks;
                bool found = false;
                for (const Entry& e : built) if (e.path == t.trackPath) { found = true; break; }
                CHECK(found);
            }
            else { ++addAlbums; CHECK(built.size() == slice.size()); }
        }
        CHECK(addTracks == slice.size() && addAlbums == 1);
    }

    // --- 4. Stale routes and degenerate sizes ------------------------------------------------------------
    {
        CHECK(MusicQueue::forArtist(idx, QStringLiteral("nobody at all")).isEmpty());
        CHECK(MusicQueue::forArtist(idx, QString()).isEmpty());
        const Index empty;
        CHECK(MusicQueue::forLibrary(empty).isEmpty());
        QVector<Entry> none;
        MusicQueue::shuffle(none, 3u);                 // must not read q[-1]
        CHECK(none.isEmpty());
        QVector<Entry> one = MusicQueue::forArtist(idx, plaid);
        CHECK(one.size() == 1);
        MusicQueue::shuffle(one, 3u);
        CHECK(one.size() == 1);
        CHECK(one.first().title == QStringLiteral("Prague Radio"));
    }

    // --- 5. The rows that reach the verbs ----------------------------------------------------------------
    // A pure cover resolver so the rows are pinned with no filesystem in the way (probe_musicbrowse's idiom).
    const browse::MusicCoverFn noArt = [](const Album&) { return QString(); };
    {
        const MediaCatalog cat = browse::musicArtistsCatalog(idx, {}, noArt);
        // The shuffle row FIRST, then the five artists.
        CHECK(cat.items.size() == 6);
        const MediaItem sh = at(cat, 0);
        CHECK(sh.type == QString::fromLatin1(browse::kMusicShuffleAllType));
        CHECK(sh.title == QStringLiteral("Shuffle all music"));
        CHECK(sh.url.isEmpty());                       // routed by type, never opened as a file
        CHECK(sh.mime == QString::fromLatin1(browse::kMusicShuffleAllPrefix));
        CHECK(browse::musicKeyOf(sh.mime, browse::kMusicShuffleAllPrefix).isEmpty());   // keyless == library
        CHECK(sh.subtitle.contains(QStringLiteral("16 track")));   // agrees with forLibrary's size
        CHECK(sh.subtitle.contains(QStringLiteral("5 artist")));
        // The artist rows still follow, unchanged and in order.
        CHECK(at(cat, 1).title == QStringLiteral("Aphex Twin"));
        CHECK(at(cat, 5).title == QStringLiteral("Various Artists"));
        CHECK(at(cat, 1).type == QString::fromLatin1(browse::kMusicArtistType));
    }
    {
        const MediaCatalog cat = browse::musicArtistCatalog(idx, aphex, noArt);
        // Play all, Shuffle all, then the three albums.
        CHECK(cat.items.size() == 5);
        const MediaItem play = at(cat, 0);
        CHECK(play.type == QString::fromLatin1(browse::kMusicPlayArtistType));
        CHECK(play.title == QStringLiteral("Play all"));
        CHECK(play.url.isEmpty());
        CHECK(browse::musicKeyOf(play.mime, browse::kMusicPlayArtistPrefix) == aphex);
        CHECK(play.subtitle.contains(QStringLiteral("3 album")));
        CHECK(play.subtitle.contains(QStringLiteral("8 track")));   // == forArtist(aphex).size()
        const MediaItem shuf = at(cat, 1);
        CHECK(shuf.type == QString::fromLatin1(browse::kMusicShuffleArtistType));
        CHECK(shuf.title == QStringLiteral("Shuffle all"));
        CHECK(browse::musicKeyOf(shuf.mime, browse::kMusicShuffleArtistPrefix) == aphex);
        CHECK(shuf.subtitle.contains(QStringLiteral("8 track")));
        CHECK(at(cat, 2).type == QString::fromLatin1(browse::kMusicAlbumType));
        CHECK(at(cat, 2).title == QStringLiteral("Ambient Works"));   // the discography still leads with 1992
    }
    {
        // The colon-carrying key round-trips through BOTH new artist prefixes, and resolves to a real queue.
        const MediaCatalog cat = browse::musicArtistCatalog(idx, prefuse, noArt);
        CHECK(cat.items.size() == 3);
        CHECK(browse::musicKeyOf(at(cat, 0).mime, browse::kMusicPlayArtistPrefix) == prefuse);
        CHECK(browse::musicKeyOf(at(cat, 1).mime, browse::kMusicShuffleArtistPrefix) == prefuse);
        CHECK(MusicQueue::forArtist(idx, browse::musicKeyOf(at(cat, 0).mime,
                                                            browse::kMusicPlayArtistPrefix)).size() == 2);
    }
    {
        // A ONE-TRACK artist gets no verbs: there is nothing to order and nothing to shuffle, and a row that
        // did the same thing as the album row under it would be noise.
        const MediaCatalog cat = browse::musicArtistCatalog(idx, plaid, noArt);
        CHECK(cat.items.size() == 1);
        CHECK(at(cat, 0).type == QString::fromLatin1(browse::kMusicAlbumType));
        // ...and an unknown key gets none either, rather than offering to play an artist that is not there.
        const MediaCatalog stale = browse::musicArtistCatalog(idx, QStringLiteral("gone"), noArt);
        CHECK(stale.items.isEmpty());
    }
    {
        // A ONE-TRACK LIBRARY has nothing to shuffle either — and an EMPTY one must still show only its
        // explanation, never a Shuffle row over nothing.
        const QString tiny = base + QStringLiteral("/tiny");
        QDir().mkpath(tiny);
        CHECK(writeMp3(tiny + QStringLiteral("/only.mp3"), QStringLiteral("Only"), QStringLiteral("Solo"),
                       QStringLiteral("Solo"), QStringLiteral("Alone"), QStringLiteral("1/1")));
        const Index one = MusicLibrary::buildIndex(MusicLibrary::scanFolder(tiny));
        CHECK(one.trackCount == 1);
        const MediaCatalog cat = browse::musicArtistsCatalog(one, {}, noArt);
        CHECK(cat.items.size() == 1);
        CHECK(cat.items.first().type == QString::fromLatin1(browse::kMusicArtistType));

        const Index none;
        const browse::MusicEmptyNote note{ QStringLiteral("No music found."), QStringLiteral("C:\\music") };
        const MediaCatalog blank = browse::musicArtistsCatalog(none, note, noArt);
        CHECK(blank.items.size() == 1);
        CHECK(blank.items.first().type == QStringLiteral("info"));
    }

    // --- 6. THE PAYOFF: crossfade over a real shuffled queue ---------------------------------------------
    // The whole reason multi-album queues were built. Crossfade.h decides per boundary; a queue that never
    // leaves one record gives it nothing to decide. Run the REAL decision over a REAL shuffled queue and pin
    // both directions: a boundary between two records is taken, a boundary inside one is refused.
    {
        QVector<Entry> q = MusicQueue::forLibrary(idx);
        MusicQueue::shuffle(q, 7u);
        const int setting = 6;   // seconds, as a user who turned it on would have it
        int faded = 0, suppressed = 0;
        for (int i = 0; i + 1 < q.size(); ++i)
        {
            const double secs = Crossfade::secondsFor(setting, /*isMusic*/ true,
                                                      facts(q[i].path), facts(q[i + 1].path));
            if (q[i].albumKey == q[i + 1].albumKey)
            {
                // Two tracks of one record landed next to each other in the shuffle: NEVER dissolve that
                // seam. This is the rule #141 calls vandalism to break, checked on the exact queue shape
                // that can now produce it.
                CHECK(secs == 0.0);
                ++suppressed;
            }
            else
            {
                // Different records: the boundary the feature exists for. (The fixtures are 2 s long, so
                // Crossfade's half-the-shorter-side cap brings the 6 s window down to 1.0 s — still a fade,
                // which is the point: the cap shortens, it does not suppress.)
                CHECK(secs > 0.0);
                CHECK(secs == 1.0);
                ++faded;
            }
        }
        // Both halves actually happened on this queue — otherwise the loop above proves nothing.
        CHECK(faded > 0);
        CHECK(suppressed > 0);
        std::printf("musicqueue: shuffled queue of %d -> %d faded boundaries, %d suppressed\n",
                    int(q.size()), faded, suppressed);

        // And the counter-case, so the assertion above is not passing for the wrong reason: the ORDERED
        // library queue keeps every record together, so the only boundaries it can fade are the few between
        // records — which is exactly why crossfade looked inert before this feature existed.
        const QVector<Entry> ordered = MusicQueue::forLibrary(idx);
        int orderedFades = 0;
        for (int i = 0; i + 1 < ordered.size(); ++i)
            if (Crossfade::secondsFor(setting, true, facts(ordered[i].path),
                                      facts(ordered[i + 1].path)) > 0.0) ++orderedFades;
        CHECK(orderedFades == idx.albumCount - 1);   // one per record change, and not one more
        CHECK(faded > orderedFades);                 // the shuffle really does create more of them
    }

    // --- 7. Twenty thousand tracks, on this thread, inside one frame -------------------------------------
    // MusicQueue.h claims the build belongs on the GUI thread because it opens nothing. This is that claim,
    // measured. Hand-built (the ONE place this probe does not scan real files) because the subject is a
    // memory walk, not twenty thousand file writes.
    {
        Index big;
        int n = 0;
        for (int ai = 0; ai < 100; ++ai)
        {
            Artist a;
            a.key  = QStringLiteral("artist%1").arg(ai, 4, 10, QLatin1Char('0'));
            a.name = a.key;
            for (int bi = 0; bi < 10; ++bi)
            {
                Album b;
                b.key   = a.key + QStringLiteral("|album%1").arg(bi);
                b.title = b.key;
                b.albumArtist = a.name;
                for (int ti = 0; ti < 20; ++ti)
                {
                    MusicLibrary::IndexTrack t;
                    t.path  = QStringLiteral("C:/music/%1/%2/%3.mp3").arg(a.key, b.key).arg(ti);
                    t.title = QStringLiteral("track %1").arg(ti);
                    t.track = ti + 1;
                    b.tracks.push_back(t);
                    ++n;
                }
                a.trackCount += int(b.tracks.size());
                a.albums.push_back(b);
                ++big.albumCount;
            }
            big.artists.push_back(a);
        }
        big.trackCount = n;
        CHECK(big.trackCount == 20000);

        QElapsedTimer timer;
        timer.start();
        QVector<Entry> q = MusicQueue::forLibrary(big);
        MusicQueue::shuffle(q, 1234u);
        const qint64 ms = timer.elapsed();
        CHECK(q.size() == 20000);
        CHECK(albumRuns(q) > big.albumCount);
        // One 60 Hz frame is 16 ms; the budget is generous by an order of magnitude so a loaded CI box
        // cannot flake it, and still an order of magnitude below anything a person would perceive as a
        // stall. If this ever fails, the answer is a worker thread and an Index copy, not a bigger number.
        std::printf("musicqueue: 20000-track build + shuffle took %lld ms\n", static_cast<long long>(ms));
        CHECK(ms < 250);
    }

    if (g_fails == 0) std::printf("MUSICQUEUE-OK\n");
    else              std::printf("MUSICQUEUE-FAILED %d\n", g_fails);
    return g_fails == 0 ? 0 : 1;
}
