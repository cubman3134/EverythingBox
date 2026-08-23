// Headless check of the local music BROWSE (src/browse/MusicCatalogs + src/core/MusicArt — issue #74,
// increment 3): the three levels a person actually walks, Artists -> that artist's Albums -> that album's
// Tracks, and the artwork behind their tiles.
//
// THE FIXTURES ARE REAL TAGGED FILES, written from tools/MusicFixtures.h — the same byte builders
// probe_musictags pins the reader against and probe_musiclibrary pins the scan against — and they are SCANNED
// by the real MusicLibrary code. Nothing here hand-builds an Index and asserts on it: a builder tested
// against a hand-built index proves only that the two agree, while the thing worth pinning is what a folder
// on somebody's disk turns into on screen. Every expected value below is a literal written next to the tag
// that produces it.
//
// What it pins:
//   1. THE ARTIST LIST. One row per artist, in the index's order, each expandable, each carrying a route id
//      that the next level can actually resolve — and NOT one row per album or per file.
//   2. AN ARTIST'S ALBUMS. Titled with the artist; a row per album; a stale/unknown artist key is an empty
//      catalog rather than a crash.
//
//      Both of those levels now LEAD with multi-album action rows ("Shuffle all music" at the root, "Play
//      all"/"Shuffle all" under an artist). What those rows DO is probe_musicqueue's subject; this probe only
//      counts past them, so that the assertions below stay about the browse levels they were written for.
//   3. AN ALBUM'S TRACKS in DISC-then-TRACK order, led by the "Play album" action row. The multi-disc fixture
//      is deliberately named so that filename order gives a different answer.
//   4. THE COMPILATION appears ONCE, under "Various Artists", as ONE album — the bug #74 names — and its
//      track rows show each track's OWN artist, which an ordinary album's rows do not.
//   5. THE EMPTY / UNCONFIGURED CASE: an empty index yields the caller's explanation as a single
//      non-actionable "info" row, never a silently empty shelf — and yields a plain empty catalog when the
//      caller passes no explanation, so the reason is never fabricated by the builder.
//   6. THE ROUTING CONTRACT round-trips: every row's mime parses back to the key that produced it, including
//      an album whose title contains a ':' (which a section()-based reader would truncate).
//   7. ARTWORK: the embedded cover is extracted off the scan thread into the cache and preferred; an album
//      with no embedded art falls back to the sibling cover.*/folder.* file; an album with neither has none.
//      Extraction is idempotent — a second pass writes nothing.
//   8. MULTI-ARTIST TRACKS (issue #196): a track credited to two people appears under EACH of them — the
//      guest gets a page of their credits rather than a phantom album, the row routes back to the record
//      it is on, and the record itself is still ONE album with both its tracks.
//   9. THE CLASSICAL VIEW (issue #196, part 2): a "Composers" door on the Music root — and NOT on a library
//      with no COMPOSER tag, which is the compatibility claim in one assertion; the composer list; a work
//      titled by its WORK tag and subtitled by who is playing; an untagged work borrowing its album's
//      title; a work's movements in TRACK order when path order disagrees; every movement routing to the
//      ALBUM it is on; the composer/conductor facts riding in art.meta for the #63 filter while an ordinary
//      track row carries no meta at all; and stale keys yielding empty, titled catalogs.
//
// Prints MUSICBROWSE-OK on success; any failure prints MUSICBROWSE-FAIL <cond> (line) and exits non-zero.
//
// Isolation: AppPaths::dataDir() is this process's own scratch dir (issue #42), so the fixture library and
// the art cache are written under it and go away at exit.
#include "MusicCatalogs.h"
#include "MusicArt.h"
#include "MusicLibrary.h"
#include "AppPaths.h"
#include "MusicFixtures.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QBuffer>
#include <QImage>
#include <QString>
#include <cstdio>

static int g_fails = 0;

#define CHECK(cond)                                                                          \
    do {                                                                                     \
        if (!(cond)) { std::printf("MUSICBROWSE-FAIL %s (%d)\n", #cond, __LINE__); ++g_fails; } \
    } while (0)

using MusicLibrary::Album;
using MusicLibrary::Artist;
using MusicLibrary::Index;
using MusicLibrary::TrackEntry;

// ---------------------------------------------------------------------------------------------------------
// Fixture writers over MusicFixtures.h's byte builders. An empty QString for a field means "write no frame
// at all", which is how a missing album artist is expressed rather than an empty one.
// ---------------------------------------------------------------------------------------------------------
static bool writeMp3(const QString& path, const QString& title, const QString& artist,
                     const QString& albumArtist, const QString& album,
                     const QString& trck = QString(), const QString& tpos = QString(),
                     const QString& year = QString(), const QByteArray& cover = QByteArray(),
                     const QByteArray& coverMime = QByteArray("image/jpeg"))
{
    QByteArray frames;
    if (!title.isEmpty())       frames.append(id3TextFrame("TIT2", title));
    if (!artist.isEmpty())      frames.append(id3TextFrame("TPE1", artist));
    if (!albumArtist.isEmpty()) frames.append(id3TextFrame("TPE2", albumArtist));
    if (!album.isEmpty())       frames.append(id3TextFrame("TALB", album));
    if (!trck.isEmpty())        frames.append(id3TextFrame("TRCK", trck));
    if (!tpos.isEmpty())        frames.append(id3TextFrame("TPOS", tpos));
    if (!year.isEmpty())        frames.append(id3TextFrame("TDRC", year));
    if (!cover.isEmpty())       frames.append(id3ApicFrame(coverMime, 0x03, cover));
    return writeFixture(path, mp3File(frames));
}

// A FLAC with a Vorbis comment block, for the classical fixtures in section 9: a repeated field is how a
// container carries several composers or performers structurally, and ID3v2.3 cannot express it at all.
// 132300 samples at 44100 Hz == a 3-second duration, hand-computed (MusicFixtures.h says so).
static bool writeFlac(const QString& path, const QList<QByteArray>& comments)
{
    QByteArray flac("fLaC", 4);
    flac.append(flacBlock(0, flacStreamInfo(44100, 2, 16, 132300), false));
    flac.append(flacBlock(4, flacVorbisComment(comments), true));
    return writeFixture(path, flac);
}

// The row at `i`, or a default-constructed item — so a wrong row COUNT fails an assertion instead of
// indexing off the end.
static MediaItem at(const MediaCatalog& c, int i)
{
    return (i >= 0 && i < c.items.size()) ? c.items[i] : MediaItem{};
}

// The first row of `c` whose title is `title` (rows are few; this keeps the assertions readable).
static MediaItem rowTitled(const MediaCatalog& c, const QString& title)
{
    for (const MediaItem& it : c.items) if (it.title == title) return it;
    return MediaItem{};
}

static QString keyOf(const QString& s) { return s.trimmed().toCaseFolded(); }

// A REAL, decodable cover image — deliberately not MusicFixtures.h's jpegBytes(), which is a JFIF magic
// number with filler behind it. That stub is exactly right for the tag reader ("a picture is present, and
// here is where it starts and stops"), and exactly wrong here: extraction DECODES the picture, so a stub
// would make the whole artwork section pass by never getting past loadFromData. Encoded in-process rather
// than committed, for the reason the byte builders are: a checked-in binary is a blob no reviewer can audit.
// 700px on purpose — larger than MusicArt::kMaxEdge, so the downscale is exercised rather than assumed.
static QByteArray realCoverPng()
{
    QImage img(700, 700, QImage::Format_RGB32);
    for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x)
            img.setPixel(x, y, qRgb(x % 256, y % 256, (x + y) % 256));   // not flat, so it cannot encode to nothing
    QByteArray out;
    QBuffer buf(&out);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, "PNG");
    return out;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    const QString base   = AppPaths::dataDir() + QStringLiteral("/musicbrowse");
    const QString root   = base + QStringLiteral("/root");
    const QString artDir = base + QStringLiteral("/art");
    QDir().mkpath(root);

    const QByteArray jpeg  = jpegBytes();      // the sibling cover.jpg: never decoded, only found
    const QByteArray embed = realCoverPng();   // the EMBEDDED cover: decoded and downscaled by MusicArt

    // --- The fixture library ---------------------------------------------------------------------------
    // A. An ordinary album with an EMBEDDED cover on its first track and no sibling image at all: the
    //    preferred artwork source, and the one that has to survive extraction to be usable.
    const QString dirA = root + QStringLiteral("/Aphex Twin/Selected Ambient Works");
    CHECK(writeMp3(dirA + QStringLiteral("/01 - Xtal.mp3"), QStringLiteral("Xtal"),
                   QStringLiteral("Aphex Twin"), QString(), QStringLiteral("Selected Ambient Works"),
                   QStringLiteral("1/2"), QString(), QStringLiteral("1992"), embed,
                   QByteArray("image/png")));
    CHECK(writeMp3(dirA + QStringLiteral("/02 - Tha.mp3"), QStringLiteral("Tha"),
                   QStringLiteral("Aphex Twin"), QString(), QStringLiteral("Selected Ambient Works"),
                   QStringLiteral("2/2"), QString(), QStringLiteral("1992")));

    // B. The COMPILATION: three track artists, one album artist, NO embedded art but a sibling cover.jpg —
    //    so this fixture carries the fallback half of the artwork rule as well as the grouping bug.
    const QString dirB = root + QStringLiteral("/Compilations/Warp Sampler");
    CHECK(writeMp3(dirB + QStringLiteral("/a.mp3"), QStringLiteral("Windowlicker"),
                   QStringLiteral("Aphex Twin"), QStringLiteral("Various Artists"),
                   QStringLiteral("Warp Sampler"), QStringLiteral("1/3"), QString(), QStringLiteral("1999")));
    CHECK(writeMp3(dirB + QStringLiteral("/b.mp3"), QStringLiteral("Roygbiv"),
                   QStringLiteral("Boards of Canada"), QStringLiteral("Various Artists"),
                   QStringLiteral("Warp Sampler"), QStringLiteral("2/3"), QString(), QStringLiteral("1999")));
    CHECK(writeMp3(dirB + QStringLiteral("/c.mp3"), QStringLiteral("Eyen"),
                   QStringLiteral("Plaid"), QStringLiteral("Various Artists"),
                   QStringLiteral("Warp Sampler"), QStringLiteral("3/3"), QString(), QStringLiteral("1999")));
    CHECK(writeFixture(dirB + QStringLiteral("/cover.jpg"), jpeg));

    // C. A MULTI-DISC album, one folder per disc, with filenames chosen so PATH order (b-01, a-02, then
    //    a-01) is NOT disc/track order — an implementation that walked the folder would fail here. It is
    //    also the album title carrying a ':', which a colon-splitting route reader would truncate.
    const QString dirC = root + QStringLiteral("/Boards of Canada/Geogaddi");
    CHECK(writeMp3(dirC + QStringLiteral("/Disc 1/b-01.mp3"), QStringLiteral("Sixtyten"),
                   QStringLiteral("Boards of Canada"), QStringLiteral("Boards of Canada"),
                   QStringLiteral("Geogaddi: Live"), QStringLiteral("1/2"), QStringLiteral("1/2"),
                   QStringLiteral("2002")));
    CHECK(writeMp3(dirC + QStringLiteral("/Disc 1/a-02.mp3"), QStringLiteral("Julie and Candy"),
                   QStringLiteral("Boards of Canada"), QStringLiteral("Boards of Canada"),
                   QStringLiteral("Geogaddi: Live"), QStringLiteral("2/2"), QStringLiteral("1/2"),
                   QStringLiteral("2002")));
    CHECK(writeMp3(dirC + QStringLiteral("/Disc 2/a-01.mp3"), QStringLiteral("Dawn Chorus"),
                   QStringLiteral("Boards of Canada"), QStringLiteral("Boards of Canada"),
                   QStringLiteral("Geogaddi: Live"), QStringLiteral("1/1"), QStringLiteral("2/2"),
                   QStringLiteral("2002")));

    // D. An album with NEITHER embedded art NOR a sibling image: the "no picture" case must be empty rather
    //    than a stale path from whichever album was resolved before it.
    const QString dirD = root + QStringLiteral("/Plaid/Not For Threes");
    CHECK(writeMp3(dirD + QStringLiteral("/01.mp3"), QStringLiteral("Prague Radio"),
                   QStringLiteral("Plaid"), QStringLiteral("Plaid"), QStringLiteral("Not For Threes"),
                   QStringLiteral("1/1"), QString(), QStringLiteral("1997")));

    const Index idx = MusicLibrary::buildIndex(MusicLibrary::scanFolder(root));
    CHECK(idx.trackCount == 9);
    CHECK(idx.albumCount == 4);

    // A pure cover resolver for the ROW assertions: the builders take one so a probe can pin the rows with
    // no filesystem in the way at all (pcGamesCatalog's injected `poster`, same reason). The artwork section
    // at the bottom exercises the real one.
    const browse::MusicCoverFn noArt = [](const Album&) { return QString(); };
    const browse::MusicCoverFn tagArt = [](const Album& b) { return QStringLiteral("art:") + b.title; };

    // --- 1. The artist list ----------------------------------------------------------------------------
    {
        const MediaCatalog cat = browse::musicArtistsCatalog(idx, {}, tagArt);
        CHECK(cat.title == QStringLiteral("Music"));
        CHECK(!cat.hasMore);
        // The "Shuffle all music" action row leads (its own probe, probe_musicqueue, owns what it does),
        // then four artists: Aphex Twin, Boards of Canada, Plaid, Various Artists — and NOT one row per
        // album (six would mean the level collapsed) nor one per file.
        CHECK(cat.items.size() == 5);
        CHECK(at(cat, 0).type == QString::fromLatin1(browse::kMusicShuffleAllType));
        CHECK(at(cat, 1).title == QStringLiteral("Aphex Twin"));
        CHECK(at(cat, 2).title == QStringLiteral("Boards of Canada"));
        CHECK(at(cat, 3).title == QStringLiteral("Plaid"));
        CHECK(at(cat, 4).title == QStringLiteral("Various Artists"));
        for (int i = 1; i < cat.items.size(); ++i)      // from 1: row 0 is the action row, not an artist
        {
            const MediaItem it = at(cat, i);
            CHECK(it.expandable);                       // every artist drills
            CHECK(it.url.isEmpty());                    // ...and none of them is a file to open
            CHECK(it.type == QString::fromLatin1(browse::kMusicArtistType));
            CHECK(it.mime.startsWith(QString::fromLatin1(browse::kMusicArtistPrefix)));
            // The route id resolves: the key on this row names a real artist in the index it came from.
            CHECK(idx.artist(browse::musicKeyOf(it.mime, browse::kMusicArtistPrefix)) != nullptr);
        }
        // Aphex Twin has ONE album of their own; appearing on the compilation must not give them a second.
        CHECK(at(cat, 1).subtitle.contains(QStringLiteral("1 album")));
        CHECK(at(cat, 1).subtitle.contains(QStringLiteral("2 track")));
        // An artist row borrows its first album's cover rather than showing a blank card.
        CHECK(at(cat, 1).thumbnailUrl == QStringLiteral("art:Selected Ambient Works"));
    }

    // --- 2. One artist's albums ------------------------------------------------------------------------
    {
        const QString bocKey = keyOf(QStringLiteral("Boards of Canada"));
        const MediaCatalog cat = browse::musicArtistCatalog(idx, bocKey, tagArt);
        CHECK(cat.title == QStringLiteral("Boards of Canada"));
        // "Play all" + "Shuffle all" (probe_musicqueue owns them) and then ONE album row: the two-disc set
        // is one album.
        CHECK(cat.items.size() == 3);
        const MediaItem alb = at(cat, 2);
        CHECK(alb.title == QStringLiteral("Geogaddi: Live"));
        CHECK(alb.expandable);
        CHECK(alb.url.isEmpty());
        CHECK(alb.type == QString::fromLatin1(browse::kMusicAlbumType));
        CHECK(alb.subtitle.contains(QStringLiteral("2002")));
        CHECK(alb.subtitle.contains(QStringLiteral("3 track")));
        CHECK(alb.subtitle.contains(QStringLiteral("2 disc")));   // ...said once, on one row
        // 6. The route id survives the ':' in the album title.
        const QString akey = browse::musicKeyOf(alb.mime, browse::kMusicAlbumPrefix);
        CHECK(akey.contains(QLatin1Char(':')));
        CHECK(idx.album(akey) != nullptr);

        // A stale route (rescanned out from under the row) is an empty catalog, not a crash.
        const MediaCatalog gone = browse::musicArtistCatalog(idx, QStringLiteral("nobody"), tagArt);
        CHECK(gone.items.isEmpty());
        CHECK(!gone.title.isEmpty());
    }

    // --- 3. An album's tracks, disc-then-track, behind a Play-album row ---------------------------------
    {
        const Artist* boc = idx.artist(keyOf(QStringLiteral("Boards of Canada")));
        CHECK(boc != nullptr);
        const QString akey = boc && !boc->albums.isEmpty() ? boc->albums.first().key : QString();
        const MediaCatalog cat = browse::musicAlbumCatalog(idx, akey, tagArt);
        CHECK(cat.title == QStringLiteral("Geogaddi: Live"));
        CHECK(cat.items.size() == 4);                   // the action row + three tracks

        const MediaItem play = at(cat, 0);
        CHECK(play.type == QString::fromLatin1(browse::kMusicPlayAlbumType));
        CHECK(play.url.isEmpty());                      // routed by mime; it is not a file
        CHECK(!play.expandable);
        CHECK(browse::musicKeyOf(play.mime, browse::kMusicPlayAlbumPrefix) == akey);
        CHECK(play.subtitle.contains(QStringLiteral("3 track")));

        // Disc order, then track order — NOT the a-01/a-02/b-01 order the filenames would give.
        CHECK(at(cat, 1).title == QStringLiteral("1-1. Sixtyten"));
        CHECK(at(cat, 2).title == QStringLiteral("1-2. Julie and Candy"));
        CHECK(at(cat, 3).title == QStringLiteral("2-1. Dawn Chorus"));
        CHECK(at(cat, 1).url.endsWith(QStringLiteral("/Disc 1/b-01.mp3")));
        CHECK(at(cat, 3).url.endsWith(QStringLiteral("/Disc 2/a-01.mp3")));
        for (int i = 1; i < cat.items.size(); ++i)
        {
            const MediaItem& t = cat.items[i];
            CHECK(t.type == QString::fromLatin1(browse::kMusicTrackType));
            CHECK(!t.url.isEmpty());                                        // a real, playable file
            CHECK(QFileInfo::exists(t.url));
            // ...and it names the album to queue behind it, which is what stops the folder queue from
            // playing one disc of a two-disc set.
            CHECK(browse::musicKeyOf(t.mime, browse::kMusicTrackPrefix) == akey);
            // An ordinary album's rows do NOT repeat the artist on every line.
            CHECK(!t.subtitle.contains(QStringLiteral("Boards of Canada")));
        }

        // An unknown album key is an empty, titled catalog.
        const MediaCatalog gone = browse::musicAlbumCatalog(idx, QStringLiteral("nope"), tagArt);
        CHECK(gone.items.isEmpty());
        CHECK(!gone.title.isEmpty());
    }

    // --- 4. The compilation: one artist, one album, per-track artists -----------------------------------
    {
        const Artist* va = idx.artist(keyOf(QStringLiteral("Various Artists")));
        CHECK(va != nullptr);
        if (va)
        {
            CHECK(va->albums.size() == 1);
            const MediaCatalog albums = browse::musicArtistCatalog(idx, va->key, tagArt);
            CHECK(albums.title == QStringLiteral("Various Artists"));
            CHECK(albums.items.size() == 3);            // the two verb rows + ONE album, not one per track
            CHECK(at(albums, 2).title == QStringLiteral("Warp Sampler"));

            const MediaCatalog tracks = browse::musicAlbumCatalog(idx, va->albums.first().key, tagArt);
            CHECK(tracks.items.size() == 4);            // Play album + three tracks
            CHECK(at(tracks, 1).title == QStringLiteral("1. Windowlicker"));   // single disc: a bare number
            CHECK(at(tracks, 2).title == QStringLiteral("2. Roygbiv"));
            CHECK(at(tracks, 3).title == QStringLiteral("3. Eyen"));
            // Each row shows its OWN artist — the only thing that makes a compilation's track list readable.
            CHECK(at(tracks, 1).subtitle.contains(QStringLiteral("Aphex Twin")));
            CHECK(at(tracks, 2).subtitle.contains(QStringLiteral("Boards of Canada")));
            CHECK(at(tracks, 3).subtitle.contains(QStringLiteral("Plaid")));
        }
        // ...and the appearance did not give Aphex Twin a second album of their own.
        const MediaCatalog ax = browse::musicArtistCatalog(idx, keyOf(QStringLiteral("Aphex Twin")), tagArt);
        CHECK(ax.items.size() == 3);                    // the two verb rows + their one own album
        CHECK(at(ax, 2).title == QStringLiteral("Selected Ambient Works"));
    }

    // --- 5. The empty / unconfigured case ---------------------------------------------------------------
    {
        const Index empty;                              // nothing scanned, or nothing found
        const browse::MusicEmptyNote note{ QStringLiteral("No music folder yet. Choose one in Settings."),
                                           QStringLiteral("D:\Music") };
        const MediaCatalog told = browse::musicArtistsCatalog(empty, note, noArt);
        CHECK(told.items.size() == 1);                  // an EXPLANATION, not an empty shelf
        CHECK(at(told, 0).type == QStringLiteral("info"));   // the surface's non-actionable guidance row
        CHECK(at(told, 0).title == note.text);
        CHECK(at(told, 0).subtitle == note.detail);     // the folder rides the SECOND line, not the sentence
        CHECK(at(told, 0).url.isEmpty());
        CHECK(!at(told, 0).expandable);

        // With no note supplied the builder invents none: an empty index is an empty catalog.
        const MediaCatalog silent = browse::musicArtistsCatalog(empty, {}, noArt);
        CHECK(silent.items.isEmpty());
        CHECK(silent.title == QStringLiteral("Music"));

        // A NON-empty index never shows the info row, however loud the note it was handed.
        const MediaCatalog real = browse::musicArtistsCatalog(idx, note, noArt);
        CHECK(real.items.size() == 5);                  // the Shuffle-all row + the four artists
        CHECK(rowTitled(real, note.text).title.isEmpty());
    }

    // --- 6. The routing contract ------------------------------------------------------------------------
    {
        CHECK(browse::musicKeyOf(QStringLiteral("musicalbum:a:b"), browse::kMusicAlbumPrefix)
              == QStringLiteral("a:b"));                     // ...everything after the prefix, colons and all
        CHECK(browse::musicKeyOf(QStringLiteral("musicartist:x"), browse::kMusicAlbumPrefix).isEmpty());
        CHECK(browse::musicKeyOf(QString(), browse::kMusicTrackPrefix).isEmpty());
    }

    // --- 7. Artwork: extract off-thread, prefer embedded, fall back to the sibling file ------------------
    {
        // Before extraction the embedded-art album has NO picture at all (no sibling image in its folder),
        // which is what makes the "after" assertion mean something.
        const Album* saw = idx.album(browse::musicKeyOf(
            at(browse::musicArtistCatalog(idx, keyOf(QStringLiteral("Aphex Twin")), noArt), 2).mime,
            browse::kMusicAlbumPrefix));
        CHECK(saw != nullptr);
        if (saw) CHECK(MusicArt::albumCover(*saw, artDir).isEmpty());

        // One album has an embedded cover; the other three have none, so exactly one file is written.
        CHECK(MusicArt::extractCovers(idx, artDir) == 1);
        CHECK(MusicArt::extractCovers(idx, artDir) == 0);   // idempotent: a rescan re-extracts nothing

        if (saw)
        {
            const QString art = MusicArt::albumCover(*saw, artDir);
            CHECK(art == MusicArt::cachedCoverPath(artDir, saw->key));
            CHECK(QFile::exists(art));
            QImage img(art);
            CHECK(!img.isNull());                            // a real, decodable image came out
            // ...and it was DOWNSCALED on the way: the source is 700px square, the cache is bounded.
            CHECK(img.width() == MusicArt::kMaxEdge && img.height() == MusicArt::kMaxEdge);
        }

        // The compilation has no embedded art but does have a sibling cover.jpg: the fallback, and it is the
        // FILE in the album's own folder rather than anything from the cache.
        const Artist* va = idx.artist(keyOf(QStringLiteral("Various Artists")));
        if (va && !va->albums.isEmpty())
        {
            const QString art = MusicArt::albumCover(va->albums.first(), artDir);
            CHECK(art == dirB + QStringLiteral("/cover.jpg"));
        }

        // Neither embedded nor sibling: empty, not a leftover path from the album resolved before it.
        const Artist* plaid = idx.artist(keyOf(QStringLiteral("Plaid")));
        if (plaid && !plaid->albums.isEmpty())
            CHECK(MusicArt::albumCover(plaid->albums.first(), artDir).isEmpty());

        // The sibling rule itself, stated directly: precedence inside a folder, and "" for a folder with none.
        CHECK(MusicArt::siblingCover(dirB) == dirB + QStringLiteral("/cover.jpg"));
        CHECK(MusicArt::siblingCover(dirD).isEmpty());
        CHECK(MusicArt::siblingCover(QString()).isEmpty());
    }

    // --- 8. MULTI-ARTIST TRACKS ON SCREEN (issue #196, part 1) ------------------------------------------
    // A separate root, scanned on its own, so the nine-file library above keeps asserting exactly what it
    // always asserted. The question here is the one a person asks with a remote in their hand: I know two
    // people are on this track, so can I find it under either of them — and is the record still one record?
    {
        const QString mroot = base + QStringLiteral("/multi");

        // One album by a duo, tagged the ID3v2.3 way: both names in ONE artist string, no album artist.
        const QString mA = mroot + QStringLiteral("/Raising Hell");
        CHECK(writeMp3(mA + QStringLiteral("/01.mp3"), QStringLiteral("Walk This Way"),
                       QStringLiteral("Run-D.M.C.; Aerosmith"), QString(), QStringLiteral("Raising Hell"),
                       QStringLiteral("1/2"), QString(), QStringLiteral("1986")));
        CHECK(writeMp3(mA + QStringLiteral("/02.mp3"), QStringLiteral("My Adidas"),
                       QStringLiteral("Run-D.M.C."), QString(), QStringLiteral("Raising Hell"),
                       QStringLiteral("2/2"), QString(), QStringLiteral("1986")));

        const Index m = MusicLibrary::buildIndex(
            MusicLibrary::scanFolder(mroot, {}, nullptr, { QStringLiteral(";") }));
        CHECK(m.trackCount == 2);
        CHECK(m.albumCount == 1);          // ONE record, whatever its tracks are credited to

        // The shelf shows BOTH names. Before #196 it showed one, spelled "Run-D.M.C.; Aerosmith".
        {
            const MediaCatalog cat = browse::musicArtistsCatalog(m, {}, tagArt);
            CHECK(cat.items.size() == 3);  // "Shuffle all music" + two artists
            CHECK(at(cat, 1).title == QStringLiteral("Aerosmith"));
            CHECK(at(cat, 2).title == QStringLiteral("Run-D.M.C."));
            CHECK(rowTitled(cat, QStringLiteral("Run-D.M.C.; Aerosmith")).title.isEmpty());
            // The guest has no record of their own but does have a track, and the row says so rather than
            // reading "0 albums · 0 tracks" beside something that plays.
            CHECK(at(cat, 1).subtitle.contains(QStringLiteral("0 album")));
            CHECK(at(cat, 1).subtitle.contains(QStringLiteral("1 track")));
            // ...and it borrows the cover of the album its credit is on, rather than being the one blank card.
            CHECK(at(cat, 1).thumbnailUrl == QStringLiteral("art:Raising Hell"));
        }

        // The DUO's own page is unchanged in shape: their album, with both tracks on it.
        {
            const MediaCatalog cat = browse::musicArtistCatalog(m, keyOf(QStringLiteral("Run-D.M.C.")), tagArt);
            CHECK(cat.title == QStringLiteral("Run-D.M.C."));
            CHECK(cat.items.size() == 3);  // Play all + Shuffle all + one album row
            CHECK(at(cat, 2).type == QString::fromLatin1(browse::kMusicAlbumType));
            CHECK(at(cat, 2).title == QStringLiteral("Raising Hell"));
        }

        // THE GUEST'S page: no albums, and the shared track as a playable row that routes to the album it is
        // on — so pressing it plays that record rather than opening a lone file.
        {
            const MediaCatalog cat = browse::musicArtistCatalog(m, keyOf(QStringLiteral("Aerosmith")), tagArt);
            CHECK(cat.title == QStringLiteral("Aerosmith"));
            CHECK(cat.items.size() == 1);  // one credit; no Play all / Shuffle all over somebody else's album
            const MediaItem it = at(cat, 0);
            CHECK(it.title == QStringLiteral("Walk This Way"));
            CHECK(it.type == QString::fromLatin1(browse::kMusicTrackType));
            CHECK(it.url.endsWith(QStringLiteral("01.mp3")));                 // a real file to play
            CHECK(it.mime.startsWith(QString::fromLatin1(browse::kMusicTrackPrefix)));
            const QString albumKey = browse::musicKeyOf(it.mime, browse::kMusicTrackPrefix);
            CHECK(m.album(albumKey) != nullptr);                              // ...and the route resolves
            if (m.album(albumKey))
            {
                CHECK(m.album(albumKey)->title == QStringLiteral("Raising Hell"));
            }
            CHECK(it.subtitle.contains(QStringLiteral("Raising Hell")));      // which record it is from
            CHECK(it.thumbnailUrl == QStringLiteral("art:Raising Hell"));
        }

        // And the album itself is untouched: one album page, both tracks, in track order.
        {
            const Artist* run = m.artist(keyOf(QStringLiteral("Run-D.M.C.")));
            CHECK(run && run->albums.size() == 1);
            if (run && run->albums.size() == 1)
            {
                const MediaCatalog cat = browse::musicAlbumCatalog(m, run->albums.first().key, tagArt);
                CHECK(cat.items.size() == 3);   // "Play album" + two tracks
                CHECK(at(cat, 1).title == QStringLiteral("1. Walk This Way"));
                CHECK(at(cat, 2).title == QStringLiteral("2. My Adidas"));
            }
        }
    }

    // --- 9. THE CLASSICAL VIEW ON SCREEN (issue #196, part 2) ------------------------------------------
    // A third root, so sections 1-8 keep asserting exactly what they always asserted. The question is the
    // one a classical listener asks with a remote in their hand: can I get to a composer at all, does the
    // shelf tell two recordings of the same piece apart, do the movements come in order, and does pressing
    // one still play the record it is on.
    {
        const QString croot = base + QStringLiteral("/classical");

        // Two recordings of ONE work by ONE composer. The Gould filenames are chosen so path order (a, b, c)
        // is not track order (2, 3, 1), or the ordering assertion below would pass on a builder that never
        // looked at a track number.
        const QString cA = croot + QStringLiteral("/Gould 1955");
        const QList<QByteArray> gould = {
            QByteArray("ALBUM=Goldberg Variations"), QByteArray("ALBUMARTIST=Glenn Gould"),
            QByteArray("ARTIST=Glenn Gould"), QByteArray("COMPOSER=Johann Sebastian Bach"),
            QByteArray("PERFORMER=Glenn Gould"), QByteArray("WORK=Goldberg Variations, BWV 988"),
            QByteArray("DATE=1955") };
        CHECK(writeFlac(cA + QStringLiteral("/a.flac"), gould + QList<QByteArray>{
            QByteArray("TITLE=Variatio 1 a 1 Clav."), QByteArray("MOVEMENTNAME=Variatio 1 a 1 Clav."),
            QByteArray("TRACKNUMBER=2") }));
        CHECK(writeFlac(cA + QStringLiteral("/b.flac"), gould + QList<QByteArray>{
            QByteArray("TITLE=Variatio 2 a 1 Clav."), QByteArray("MOVEMENTNAME=Variatio 2 a 1 Clav."),
            QByteArray("TRACKNUMBER=3") }));
        CHECK(writeFlac(cA + QStringLiteral("/c.flac"), gould + QList<QByteArray>{
            QByteArray("TITLE=Aria"), QByteArray("MOVEMENTNAME=Aria"), QByteArray("TRACKNUMBER=1") }));

        const QString cB = croot + QStringLiteral("/Requiem");
        CHECK(writeFlac(cB + QStringLiteral("/01.flac"), {
            QByteArray("TITLE=Lacrimosa"), QByteArray("ALBUM=Requiem"),
            QByteArray("ALBUMARTIST=Monteverdi Choir"), QByteArray("ARTIST=Monteverdi Choir"),
            QByteArray("COMPOSER=Wolfgang Amadeus Mozart"),
            QByteArray("CONDUCTOR=John Eliot Gardiner"),
            QByteArray("PERFORMER=Monteverdi Choir"), QByteArray("TRACKNUMBER=1"), QByteArray("DATE=1986") }));

        // The POP TRACK, in the same root and scanned by the same pass.
        CHECK(writeMp3(croot + QStringLiteral("/Dummy/01.mp3"), QStringLiteral("Glory Box"),
                       QStringLiteral("Portishead"), QStringLiteral("Portishead"),
                       QStringLiteral("Dummy"), QStringLiteral("1/1"), QString(),
                       QStringLiteral("1994")));

        const Index ci = MusicLibrary::buildIndex(
            MusicLibrary::scanFolder(croot, {}, nullptr, { QStringLiteral(";") }));
        CHECK(ci.trackCount == 5);
        CHECK(ci.composers.size() == 2);

        // 9a. THE DOOR. One "Composers" row on the Music root, between the shuffle row and the artists,
        //     expandable, routing to the composer list.
        {
            const MediaCatalog cat = browse::musicArtistsCatalog(ci, {}, tagArt);
            const MediaItem door = at(cat, 1);
            CHECK(door.type == QString::fromLatin1(browse::kMusicComposersType));
            CHECK(door.title == QStringLiteral("Composers"));
            CHECK(door.expandable);
            CHECK(door.mime == QString::fromLatin1(browse::kMusicComposersPrefix));
            CHECK(door.subtitle.contains(QStringLiteral("2 composer")));
            CHECK(door.subtitle.contains(QStringLiteral("2 work")));
            CHECK(!door.thumbnailUrl.isEmpty());        // borrows a cover rather than being a blank card
            CHECK(at(cat, 2).type == QString::fromLatin1(browse::kMusicArtistType));   // artists follow
        }

        // 9b. AND NOT ON A POP LIBRARY. The main nine-file fixture at the top of this probe has no COMPOSER
        //     tag anywhere, so its Music root is exactly the catalog it was before this existed: no door, no
        //     extra row, nothing shifted by one. This is the assertion the increment lives or dies by.
        {
            const MediaCatalog cat = browse::musicArtistsCatalog(idx, {}, tagArt);
            CHECK(rowTitled(cat, QStringLiteral("Composers")).title.isEmpty());
            for (const MediaItem& it : cat.items)
                CHECK(it.type != QString::fromLatin1(browse::kMusicComposersType));
            CHECK(at(cat, 1).type == QString::fromLatin1(browse::kMusicArtistType));
        }

        // 9c. THE COMPOSER LIST: one row each, alphabetically, each routing to a resolvable key.
        {
            const MediaCatalog cat = browse::musicComposersCatalog(ci, tagArt);
            CHECK(cat.title == QStringLiteral("Composers"));
            CHECK(cat.items.size() == 2);
            CHECK(at(cat, 0).title == QStringLiteral("Johann Sebastian Bach"));
            CHECK(at(cat, 1).title == QStringLiteral("Wolfgang Amadeus Mozart"));
            CHECK(at(cat, 0).type == QString::fromLatin1(browse::kMusicComposerType));
            CHECK(at(cat, 0).expandable);
            CHECK(at(cat, 0).subtitle.contains(QStringLiteral("1 work")));
            CHECK(at(cat, 0).subtitle.contains(QStringLiteral("3 track")));
            CHECK(at(cat, 0).thumbnailUrl == QStringLiteral("art:Goldberg Variations"));
            const QString ck = browse::musicKeyOf(at(cat, 0).mime, browse::kMusicComposerPrefix);
            CHECK(ci.composer(ck) != nullptr);                       // the route round-trips
        }

        // 9d. ONE COMPOSER'S WORKS. Bach's Goldbergs are titled by the WORK tag and subtitled by WHO IS
        //     PLAYING, which is the fact that makes a shelf of the same piece navigable at all.
        {
            const QString bachKey = keyOf(QStringLiteral("Johann Sebastian Bach"));
            const MediaCatalog cat = browse::musicComposerCatalog(ci, bachKey, tagArt);
            CHECK(cat.title == QStringLiteral("Johann Sebastian Bach"));
            CHECK(cat.items.size() == 1);
            const MediaItem w = at(cat, 0);
            CHECK(w.title == QStringLiteral("Goldberg Variations, BWV 988"));   // the WORK, not the album
            CHECK(w.type == QString::fromLatin1(browse::kMusicWorkType));
            CHECK(w.expandable);
            CHECK(w.subtitle.startsWith(QStringLiteral("Glenn Gould")));
            CHECK(w.subtitle.contains(QStringLiteral("3 track")));
            CHECK(w.subtitle.contains(QStringLiteral("0:09")));                 // three 3-second FLACs
            CHECK(w.thumbnailUrl == QStringLiteral("art:Goldberg Variations"));
            CHECK(ci.work(browse::musicKeyOf(w.mime, browse::kMusicWorkPrefix)) != nullptr);
        }

        // 9e. An UNTAGGED work borrows its ALBUM's title, and a conductor is "who is playing" when the file
        //     names no performer of its own beyond the choir.
        {
            const MediaCatalog cat = browse::musicComposerCatalog(
                ci, keyOf(QStringLiteral("Wolfgang Amadeus Mozart")), tagArt);
            CHECK(cat.items.size() == 1);
            CHECK(at(cat, 0).title == QStringLiteral("Requiem"));
            CHECK(at(cat, 0).subtitle.contains(QStringLiteral("John Eliot Gardiner")));
        }

        // 9f. ONE WORK'S TRACKS, in DISC-then-TRACK order rather than the a/b/c path order the walk found
        //     them in, titled by MOVEMENT, and each one routing to the ALBUM it is on - so pressing a
        //     movement plays that record from there rather than opening a lone file.
        {
            const MusicLibrary::Composer* bach = ci.composer(keyOf(QStringLiteral("Johann Sebastian Bach")));
            CHECK(bach && bach->works.size() == 1);
            if (bach && bach->works.size() == 1)
            {
                const MediaCatalog cat = browse::musicWorkCatalog(ci, bach->works.first().key, tagArt);
                CHECK(cat.title == QStringLiteral("Goldberg Variations, BWV 988"));
                CHECK(cat.items.size() == 3);                    // no action row: see MusicCatalogs.cpp
                CHECK(at(cat, 0).title == QStringLiteral("1. Aria"));
                CHECK(at(cat, 1).title == QStringLiteral("2. Variatio 1 a 1 Clav."));
                CHECK(at(cat, 2).title == QStringLiteral("3. Variatio 2 a 1 Clav."));
                CHECK(at(cat, 0).type == QString::fromLatin1(browse::kMusicTrackType));
                CHECK(at(cat, 0).url.endsWith(QStringLiteral("c.flac")));       // track 1 IS the c file
                CHECK(at(cat, 0).subtitle.startsWith(QStringLiteral("Glenn Gould")));
                CHECK(at(cat, 0).thumbnailUrl == QStringLiteral("art:Goldberg Variations"));
                const QString albumKey = browse::musicKeyOf(at(cat, 0).mime, browse::kMusicTrackPrefix);
                CHECK(ci.album(albumKey) != nullptr);
                if (ci.album(albumKey))
                    CHECK(ci.album(albumKey)->title == QStringLiteral("Goldberg Variations"));
            }
        }

        // 9g. THE FILTER CHANNEL (#63). A classical track row carries its composers and conductors in
        //     art.meta - the same bag HomeView::gameFactsFor reads a game's scraped facts out of - as BOTH
        //     an already-split list (what the evaluator matches on) and a joined display string (what a
        //     theme's meta panel shows). A POP track row carries neither, and its MediaArt stays empty.
        {
            const MusicLibrary::Composer* moz = ci.composer(keyOf(QStringLiteral("Wolfgang Amadeus Mozart")));
            CHECK(moz && moz->works.size() == 1);
            if (moz && moz->works.size() == 1)
            {
                const MediaCatalog cat = browse::musicWorkCatalog(ci, moz->works.first().key, tagArt);
                const MediaItem t = at(cat, 0);
                CHECK(t.art.meta.value(QStringLiteral("composers")).toStringList()
                      == QStringList({ QStringLiteral("Wolfgang Amadeus Mozart") }));
                CHECK(t.art.meta.value(QStringLiteral("conductors")).toStringList()
                      == QStringList({ QStringLiteral("John Eliot Gardiner") }));
                CHECK(t.art.meta.value(QStringLiteral("composer")).toString()
                      == QStringLiteral("Wolfgang Amadeus Mozart"));
            }
            const Artist* port = ci.artist(keyOf(QStringLiteral("Portishead")));
            CHECK(port && port->albums.size() == 1);
            if (port && port->albums.size() == 1)
            {
                const MediaCatalog cat = browse::musicAlbumCatalog(ci, port->albums.first().key, tagArt);
                CHECK(cat.items.size() == 2);                     // "Play album" + the one track
                CHECK(at(cat, 1).title == QStringLiteral("1. Glory Box"));
                CHECK(at(cat, 1).art.isEmpty());                  // no meta at all on an ordinary track
            }
        }

        // 9h. STALE ROUTES are empty, titled catalogs - never a crash. A rescan can delete a composer under
        //     a user who is standing in their page, and the surface re-reads the index on Back.
        {
            CHECK(browse::musicComposerCatalog(ci, QStringLiteral("nobody"), tagArt).items.isEmpty());
            CHECK(browse::musicComposerCatalog(ci, QStringLiteral("nobody"), tagArt).title
                  == QStringLiteral("Composers"));
            CHECK(browse::musicWorkCatalog(ci, QStringLiteral("nothing"), tagArt).items.isEmpty());
            CHECK(browse::musicComposersCatalog(Index{}, tagArt).items.isEmpty());
        }
    }

    if (g_fails == 0) std::printf("MUSICBROWSE-OK\n");
    return g_fails == 0 ? 0 : 1;
}
