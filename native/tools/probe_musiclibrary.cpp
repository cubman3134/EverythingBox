// Headless check of the local MUSIC LIBRARY scan and index (src/core/MusicLibrary — issue #74, increment 2):
// the layer that turns a folder of files into Artists -> Albums -> Tracks, which is what increment 3 browses.
//
// THE FIXTURES ARE REAL TAGGED FILES, written to this process's own scratch dataDir() from the byte builders
// in tools/MusicFixtures.h — the SAME builders probe_musictags pins the tag reader against. Nothing here
// fabricates a TrackEntry by hand and asserts on it: every entry below came out of AudioTags::read() parsing
// an ID3v2 / Vorbis-comment / MP4 tag block off a disk, so what is proven is the path a user's folder takes.
//
// What it pins, in the order #74 cares about:
//   1. A NORMAL album across two containers (mp3 + flac, one of them with no ALBUMARTIST at all) is one
//      artist and one album, its tracks in track order.
//   2. A COMPILATION — every track a different artist, one album artist — is ONE artist holding ONE album.
//      This is the bug #74 names: grouping on `artist` shatters it into one album per track, and the probe
//      says so twice, from the compilation's side and from the side of an artist who appears on it and must
//      NOT gain a second album because of that appearance.
//   3. A MULTI-DISC album split across two subfolders is ONE album with two discs, ordered disc-then-track —
//      and the filenames are chosen so that path order gives a DIFFERENT answer, or the assertion would pass
//      on a scan that never looked at a tag.
//   4. Two artists with the same album title are TWO albums; two spellings of one artist are ONE artist.
//   5. UNTAGGED files are reachable: their own artist bucket (sorted last), an album named after the folder
//      they live in, a title from the filename — and two untagged folders stay two albums.
//   6. The INCREMENTAL rescan does not re-read an unchanged file. Proven observationally, not by trusting a
//      counter: the file's tag is rewritten in place at the same length with its mtime restored, and the
//      rescan must still report the OLD title. Then the mtime is advanced and the new title must appear.
//   7. The persisted index round-trips (including ReplayGain presence, where 0 dB is a value and absence is
//      not), and a file written by a future version loads as empty rather than as garbage.
//   8. Nothing configured / a missing root / an empty library are dormant and instant, not errors.
//   9. MULTI-VALUE ARTISTS AND GENRES (issue #196), on a root of their own so the library above keeps
//      asserting what it always did: a track credited to several people is filed under the FIRST of them and
//      reachable from each of the others through Artist::credits, while its album stays ONE album; AC/DC is
//      one artist; a compilation of single-valued performers mints no credits at all; the separator list is a
//      real setting in both directions; and both the values and the list they were parsed with round-trip.
//
// Prints MUSICLIB-OK on success; any failure prints MUSICLIB-FAIL <cond> (line) and exits non-zero.
//
// Isolation: AppPaths::dataDir() is this process's own scratch dir (issue #42), so the whole fixture library
// is written under it and goes away at exit. Nothing is written beside the exe.
#include "MusicLibrary.h"
#include "AppPaths.h"
#include "Settings.h"   // the DEFAULT separator list is a setting, and section 11 pins it
#include "MusicFixtures.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <cstdio>

static int g_fails = 0;

#define CHECK(cond)                                                                     \
    do {                                                                                \
        if (!(cond)) { std::printf("MUSICLIB-FAIL %s (%d)\n", #cond, __LINE__); ++g_fails; } \
    } while (0)

using MusicLibrary::Album;
using MusicLibrary::Artist;
using MusicLibrary::Index;
using MusicLibrary::IndexTrack;
using MusicLibrary::ScanStats;
using MusicLibrary::TrackEntry;

// ---------------------------------------------------------------------------------------------------------
// Fixture writers. Each one builds a complete, valid file of its container out of MusicFixtures.h and drops
// it at `path`; the tag values passed in are the same literals the assertions below are written against.
// ---------------------------------------------------------------------------------------------------------

// An mp3 with an ID3v2.4 tag. An empty QString for a field means "do not write that frame at all", which is
// how the fixtures express a missing album artist rather than an empty one.
static bool writeMp3(const QString& path, const QString& title, const QString& artist,
                     const QString& albumArtist, const QString& album,
                     const QString& trck = QString(), const QString& tpos = QString(),
                     const QString& year = QString(), const QByteArray& cover = QByteArray(),
                     const QString& trackGain = QString())
{
    QByteArray frames;
    if (!title.isEmpty())       frames.append(id3TextFrame("TIT2", title));
    if (!artist.isEmpty())      frames.append(id3TextFrame("TPE1", artist));
    if (!albumArtist.isEmpty()) frames.append(id3TextFrame("TPE2", albumArtist));
    if (!album.isEmpty())       frames.append(id3TextFrame("TALB", album));
    if (!trck.isEmpty())        frames.append(id3TextFrame("TRCK", trck));
    if (!tpos.isEmpty())        frames.append(id3TextFrame("TPOS", tpos));
    if (!year.isEmpty())        frames.append(id3TextFrame("TDRC", year));
    if (!trackGain.isEmpty())   frames.append(id3TxxxFrame(QStringLiteral("REPLAYGAIN_TRACK_GAIN"), trackGain));
    if (!cover.isEmpty())       frames.append(id3ApicFrame("image/jpeg", 0x03, cover));
    return writeFixture(path, mp3File(frames));
}

// A FLAC with a Vorbis comment block. 132300 samples at 44100 Hz == a 3-second duration, hand-computed.
static bool writeFlac(const QString& path, const QList<QByteArray>& comments)
{
    QByteArray flac("fLaC", 4);
    flac.append(flacBlock(0, flacStreamInfo(44100, 2, 16, 132300), false));
    flac.append(flacBlock(4, flacVorbisComment(comments), true));
    return writeFixture(path, flac);
}

// An m4a with an iTunes ilst. No trak/mdhd, so its duration reads as 0 — see m4aFile().
static bool writeM4a(const QString& path, const QString& title, const QString& artist,
                     const QString& albumArtist, const QString& album, quint16 track, quint16 trackTotal)
{
    QByteArray ilst;
    ilst.append(mp4TextItem(itunesName("nam"), title));
    ilst.append(mp4TextItem(itunesName("ART"), artist));
    ilst.append(mp4TextItem("aART", albumArtist));
    ilst.append(mp4TextItem(itunesName("alb"), album));
    ilst.append(mp4PairItem("trkn", track, trackTotal));
    return writeFixture(path, m4aFile(ilst));
}

// Force a file's modification time. Both halves of the incremental proof need this: restoring the OLD mtime
// after an in-place rewrite (so "unchanged" is a fact and not an accident of the clock's one-second
// resolution), and advancing it to make a file changed on demand instead of waiting on wall time.
static bool setMtime(const QString& path, qint64 secs)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadWrite)) return false;
    return f.setFileTime(QDateTime::fromSecsSinceEpoch(secs), QFileDevice::FileModificationTime);
}

// The album a given artist's Nth album is, with a null-safe lookup so a wrong grouping fails an assertion
// instead of dereferencing nothing.
static const Album* albumOf(const Index& idx, const QString& artistKey, int n)
{
    const Artist* a = idx.artist(artistKey);
    if (!a || n < 0 || n >= a->albums.size()) return nullptr;
    return &a->albums[n];
}

static QString keyOf(const QString& s) { return s.trimmed().toCaseFolded(); }

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    const QString base = AppPaths::dataDir() + QStringLiteral("/musiclib");
    const QString root = base + QStringLiteral("/root");
    QDir().mkpath(root);

    const QByteArray jpeg = jpegBytes();

    // --- The fixture library -------------------------------------------------------------------------
    // 1. A normal album, deliberately MIXED CONTAINER: two mp3s that tag TPE1 only, and a flac with no
    //    ALBUMARTIST either. All three must land in one album via effectiveAlbumArtist()'s fallback, which
    //    is the ordinary case and the one a per-container grouping rule would break.
    const QString dirA = root + QStringLiteral("/Aphex Twin/Selected Ambient Works");
    CHECK(writeMp3(dirA + QStringLiteral("/01 - Xtal.mp3"), QStringLiteral("Xtal"),
                   QStringLiteral("Aphex Twin"), QString(), QStringLiteral("Selected Ambient Works"),
                   QStringLiteral("1/3"), QString(), QStringLiteral("1992"), QByteArray(),
                   QStringLiteral("-4.50 dB")));
    CHECK(writeMp3(dirA + QStringLiteral("/02 - Tha.mp3"), QStringLiteral("Tha"),
                   QStringLiteral("Aphex Twin"), QString(), QStringLiteral("Selected Ambient Works"),
                   QStringLiteral("2/3"), QString(), QStringLiteral("1992")));
    CHECK(writeFlac(dirA + QStringLiteral("/03 - Ageispolis.flac"), {
        QByteArray("TITLE=Ageispolis"), QByteArray("ARTIST=Aphex Twin"),
        QByteArray("ALBUM=Selected Ambient Works"), QByteArray("TRACKNUMBER=3"), QByteArray("DATE=1992") }));

    // 2. The COMPILATION. Three different track artists, one album artist, three different containers.
    //    One of those track artists is "Aphex Twin", who also has the album above — so this fixture also
    //    proves that appearing on a compilation does not give an artist a second album of their own.
    const QString dirB = root + QStringLiteral("/Compilations/Warp Sampler");
    CHECK(writeMp3(dirB + QStringLiteral("/a.mp3"), QStringLiteral("Windowlicker"),
                   QStringLiteral("Aphex Twin"), QStringLiteral("Various Artists"),
                   QStringLiteral("Warp Sampler"), QStringLiteral("1/3"), QString(),
                   QStringLiteral("1999"), jpeg));
    CHECK(writeMp3(dirB + QStringLiteral("/b.mp3"), QStringLiteral("Roygbiv"),
                   QStringLiteral("Boards of Canada"), QStringLiteral("Various Artists"),
                   QStringLiteral("Warp Sampler"), QStringLiteral("2/3"), QString(), QStringLiteral("1999")));
    CHECK(writeM4a(dirB + QStringLiteral("/c.m4a"), QStringLiteral("Reckoner"), QStringLiteral("Plaid"),
                   QStringLiteral("Various Artists"), QStringLiteral("Warp Sampler"), 3, 3));
    // Non-audio siblings a real album folder always has. The extension filter must skip them before opening.
    CHECK(writeFixture(dirB + QStringLiteral("/cover.jpg"), jpeg));
    CHECK(writeFixture(root + QStringLiteral("/notes.txt"), QByteArray("not music")));

    // 3. A MULTI-DISC album, one folder per disc. The filenames are chosen so that PATH order (a-02, b-01,
    //    a-01) is not track order (b-01, a-02, a-01): an implementation that sorted by filename, or that
    //    ignored TPOS, gets a different list and the assertion below catches it.
    const QString dirC = root + QStringLiteral("/Boards of Canada/Geogaddi");
    CHECK(writeMp3(dirC + QStringLiteral("/Disc 1/b-01.mp3"), QStringLiteral("Sixtyten"),
                   QStringLiteral("Boards of Canada"), QStringLiteral("Boards of Canada"),
                   QStringLiteral("Geogaddi"), QStringLiteral("1/2"), QStringLiteral("1/2"),
                   QStringLiteral("2002")));
    CHECK(writeMp3(dirC + QStringLiteral("/Disc 1/a-02.mp3"), QStringLiteral("Julie and Candy"),
                   QStringLiteral("Boards of Canada"), QStringLiteral("Boards of Canada"),
                   QStringLiteral("Geogaddi"), QStringLiteral("2/2"), QStringLiteral("1/2"),
                   QStringLiteral("2002")));
    CHECK(writeMp3(dirC + QStringLiteral("/Disc 2/a-01.mp3"), QStringLiteral("Dawn Chorus"),
                   QStringLiteral("Boards of Canada"), QStringLiteral("Boards of Canada"),
                   QStringLiteral("Geogaddi"), QStringLiteral("1/1"), QStringLiteral("2/2"),
                   QStringLiteral("2002")));

    // 4a. Two artists, ONE album title, in ONE folder. The album title alone is not a key.
    const QString dirD = root + QStringLiteral("/Live");
    CHECK(writeMp3(dirD + QStringLiteral("/alpha.mp3"), QStringLiteral("Opener"), QStringLiteral("Alpha"),
                   QString(), QStringLiteral("Live At Leeds")));
    CHECK(writeMp3(dirD + QStringLiteral("/beta.mp3"), QStringLiteral("Closer"), QStringLiteral("Beta"),
                   QString(), QStringLiteral("Live At Leeds")));

    // 4b. ONE artist, two capitalisations, and the album title cased differently too. Taggers disagree about
    //     case across a library assembled over years; three "The Orb" rows in a browse is the same class of
    //     bug as the shattered compilation.
    const QString dirE = root + QStringLiteral("/The Orb");
    CHECK(writeMp3(dirE + QStringLiteral("/x.mp3"), QStringLiteral("Little Fluffy Clouds"),
                   QStringLiteral("The Orb"), QStringLiteral("The Orb"), QStringLiteral("Orbus Terrarum"),
                   QStringLiteral("1/2")));
    CHECK(writeMp3(dirE + QStringLiteral("/y.mp3"), QStringLiteral("Oxbow Lakes"),
                   QStringLiteral("THE ORB"), QStringLiteral("THE ORB"), QStringLiteral("orbus terrarum"),
                   QStringLiteral("2/2")));

    // 5. UNTAGGED files: bare MPEG frames, no tag block at all. Three in one folder with numbers that read
    //    differently lexicographically than numerically, and one in a second folder — because the whole point
    //    of the folder fallback is that two untagged folders do not collapse into one heap.
    const QString dirF = root + QStringLiteral("/Unsorted Rips");
    CHECK(writeFixture(dirF + QStringLiteral("/1 rip.mp3"), mpegAudio()));
    CHECK(writeFixture(dirF + QStringLiteral("/02 rip.mp3"), mpegAudio()));
    CHECK(writeFixture(dirF + QStringLiteral("/10 rip.mp3"), mpegAudio()));
    const QString dirG = root + QStringLiteral("/More Rips");
    CHECK(writeFixture(dirG + QStringLiteral("/loose.mp3"), mpegAudio()));

    const int kTotalTracks = 17;

    // --- 1. The scan ----------------------------------------------------------------------------------
    ScanStats s1;
    QVector<TrackEntry> entries = MusicLibrary::scanFolder(root, {}, &s1);
    CHECK(entries.size() == kTotalTracks);
    CHECK(s1.files == kTotalTracks);
    CHECK(s1.retagged == kTotalTracks);   // nothing known yet: every file is opened exactly once
    CHECK(s1.reused == 0);
    CHECK(s1.dropped == 0);
    // cover.jpg and notes.txt are in the tree and are not in the scan.
    for (const TrackEntry& e : entries)
        CHECK(!e.path.endsWith(QStringLiteral(".jpg")) && !e.path.endsWith(QStringLiteral(".txt")));

    const Index idx = MusicLibrary::buildIndex(entries);
    CHECK(idx.trackCount == kTotalTracks);
    CHECK(idx.albumCount == 8);
    CHECK(!idx.isEmpty());

    // Artist order: natural and case-insensitive, with the UNKNOWN bucket last.
    CHECK(idx.artists.size() == 7);
    if (idx.artists.size() == 7)
    {
        CHECK(idx.artists[0].name == QStringLiteral("Alpha"));
        CHECK(idx.artists[1].name == QStringLiteral("Aphex Twin"));
        CHECK(idx.artists[2].name == QStringLiteral("Beta"));
        CHECK(idx.artists[3].name == QStringLiteral("Boards of Canada"));
        CHECK(idx.artists[4].name == QStringLiteral("The Orb"));
        CHECK(idx.artists[5].name == QStringLiteral("Various Artists"));
        CHECK(idx.artists[6].name.isEmpty());        // untagged, and LAST
    }

    // --- 2. The normal album: mixed containers, no album artist tagged anywhere ------------------------
    {
        const Artist* a = idx.artist(keyOf(QStringLiteral("Aphex Twin")));
        CHECK(a != nullptr);
        if (a)
        {
            CHECK(a->albums.size() == 1);            // and NOT two — see the compilation section below
            CHECK(a->trackCount == 3);
            const Album* b = albumOf(idx, a->key, 0);
            CHECK(b != nullptr);
            if (b)
            {
                CHECK(b->title == QStringLiteral("Selected Ambient Works"));
                CHECK(b->albumArtist == QStringLiteral("Aphex Twin"));   // via the fallback, not a tag
                CHECK(!b->titleFromFolder);
                CHECK(b->year == 1992);
                CHECK(b->discCount == 1);
                CHECK(b->durationSec == 7);          // 2 + 2 (mp3, CBR) + 3 (flac, 132300/44100)
                CHECK(b->tracks.size() == 3);
                if (b->tracks.size() == 3)
                {
                    CHECK(b->tracks[0].title == QStringLiteral("Xtal"));
                    CHECK(b->tracks[1].title == QStringLiteral("Tha"));
                    CHECK(b->tracks[2].title == QStringLiteral("Ageispolis"));   // the flac, in track order
                    CHECK(b->tracks[2].durationSec == 3);
                }
                CHECK(idx.album(b->key) == b);        // the album key is a working route id
            }
        }
    }

    // --- 3. The COMPILATION — the bug #74 names -------------------------------------------------------
    {
        const Artist* va = idx.artist(keyOf(QStringLiteral("Various Artists")));
        CHECK(va != nullptr);
        if (va)
        {
            CHECK(va->name == QStringLiteral("Various Artists"));
            CHECK(va->albums.size() == 1);           // ONE album, not one per track
            CHECK(va->trackCount == 3);
            const Album* b = albumOf(idx, va->key, 0);
            CHECK(b != nullptr);
            if (b)
            {
                CHECK(b->title == QStringLiteral("Warp Sampler"));
                CHECK(b->tracks.size() == 3);
                if (b->tracks.size() == 3)
                {
                    // Track order comes from the tags, and each track keeps its OWN artist — a compilation's
                    // track list is unreadable if every row says "Various Artists".
                    CHECK(b->tracks[0].title == QStringLiteral("Windowlicker"));
                    CHECK(b->tracks[0].artist == QStringLiteral("Aphex Twin"));
                    CHECK(b->tracks[0].hasCover);                 // the APIC on this one file
                    CHECK(b->tracks[1].artist == QStringLiteral("Boards of Canada"));
                    CHECK(b->tracks[2].artist == QStringLiteral("Plaid"));
                    CHECK(!b->tracks[1].hasCover);
                }
            }
        }
        // The other half of the same bug, said from the artists' side: two of the three track artists have
        // albums of their own elsewhere in this library, and appearing here must not have added an album to
        // either of them. Grouping on `artist` puts a one-track "Warp Sampler" under each.
        const Artist* at = idx.artist(keyOf(QStringLiteral("Aphex Twin")));
        const Artist* bc = idx.artist(keyOf(QStringLiteral("Boards of Canada")));
        CHECK(at && at->albums.size() == 1);
        CHECK(bc && bc->albums.size() == 1);
        CHECK(idx.artist(keyOf(QStringLiteral("Plaid"))) == nullptr);   // no album of their own, no bucket
    }

    // --- 4. MULTI-DISC: one album, two discs, ordered disc-then-track ---------------------------------
    {
        const Artist* bc = idx.artist(keyOf(QStringLiteral("Boards of Canada")));
        CHECK(bc != nullptr);
        if (bc)
        {
            CHECK(bc->albums.size() == 1);           // two subfolders, ONE album
            const Album* b = albumOf(idx, bc->key, 0);
            CHECK(b != nullptr);
            if (b)
            {
                CHECK(b->title == QStringLiteral("Geogaddi"));
                CHECK(b->discCount == 2);
                CHECK(b->tracks.size() == 3);
                if (b->tracks.size() == 3)
                {
                    // Path order would be Julie(a-02), Sixtyten(b-01), Dawn(a-01). Tag order is not that.
                    CHECK(b->tracks[0].title == QStringLiteral("Sixtyten"));
                    CHECK(b->tracks[0].disc == 1 && b->tracks[0].track == 1);
                    CHECK(b->tracks[1].title == QStringLiteral("Julie and Candy"));
                    CHECK(b->tracks[1].disc == 1 && b->tracks[1].track == 2);
                    CHECK(b->tracks[2].title == QStringLiteral("Dawn Chorus"));
                    CHECK(b->tracks[2].disc == 2 && b->tracks[2].track == 1);
                }
            }
        }
    }

    // --- 5. Same album title, two artists => two albums; two spellings, one artist ---------------------
    {
        const Artist* al = idx.artist(keyOf(QStringLiteral("Alpha")));
        const Artist* be = idx.artist(keyOf(QStringLiteral("Beta")));
        CHECK(al && al->albums.size() == 1 && al->albums[0].title == QStringLiteral("Live At Leeds"));
        CHECK(be && be->albums.size() == 1 && be->albums[0].title == QStringLiteral("Live At Leeds"));
        CHECK(al && be && al->albums[0].key != be->albums[0].key);   // same title, different albums

        const Artist* orb = idx.artist(keyOf(QStringLiteral("the orb")));
        CHECK(orb != nullptr);
        if (orb)
        {
            CHECK(orb->name == QStringLiteral("The Orb"));   // first spelling in path order wins the display
            CHECK(orb->albums.size() == 1);                  // "Orbus Terrarum" and "orbus terrarum" are one
            CHECK(orb->albums[0].title == QStringLiteral("Orbus Terrarum"));
            CHECK(orb->albums[0].tracks.size() == 2);
        }
        // The two capitalisations resolve to the SAME key, which is what made them one artist.
        CHECK(idx.artist(keyOf(QStringLiteral("THE ORB"))) == orb);
    }

    // --- 6. UNTAGGED files are reachable, and not one heap ---------------------------------------------
    {
        const Artist* un = idx.artist(QString());     // the unknown-artist key is the empty string
        CHECK(un != nullptr);
        if (un)
        {
            CHECK(un->name.isEmpty());
            CHECK(un->trackCount == 4);
            CHECK(un->albums.size() == 2);            // two untagged FOLDERS stay two albums
            CHECK(!MusicLibrary::displayArtist(*un).isEmpty());   // the UI wording exists...
            CHECK(MusicLibrary::displayArtist(*un) != un->name);  // ...and is not the stored empty name

            // Both albums have no year, so they sort by title: "More Rips" before "Unsorted Rips".
            const Album* more = albumOf(idx, un->key, 0);
            const Album* uns  = albumOf(idx, un->key, 1);
            CHECK(more && more->title == QStringLiteral("More Rips"));
            CHECK(more && more->titleFromFolder);
            CHECK(uns && uns->title == QStringLiteral("Unsorted Rips"));
            CHECK(uns && uns->titleFromFolder);
            CHECK(more && uns && more->key != uns->key);
            if (uns)
            {
                CHECK(uns->tracks.size() == 3);
                if (uns->tracks.size() == 3)
                {
                    // Every track number is 0, so the tiebreak is the filename — and it is NATURAL order,
                    // so "1 rip" precedes "02 rip" precedes "10 rip" rather than sorting as text.
                    CHECK(uns->tracks[0].title == QStringLiteral("1 rip"));
                    CHECK(uns->tracks[1].title == QStringLiteral("02 rip"));
                    CHECK(uns->tracks[2].title == QStringLiteral("10 rip"));
                    CHECK(uns->tracks[0].track == 0);       // filename title, not an invented number
                }
                CHECK(uns->folder == QFileInfo(dirF).absoluteFilePath());
                CHECK(!MusicLibrary::displayAlbum(*uns).isEmpty());
            }
        }
        // The scan recorded them as untagged, which is AudioTags' own verdict carried through.
        int untaggedCount = 0;
        for (const TrackEntry& e : entries) if (e.untagged) ++untaggedCount;
        CHECK(untaggedCount == 4);
    }

    // --- 7. ReplayGain survives the scan (it was read in the same pass; #141 never rescans for it) ------
    {
        const QString gainPath = QFileInfo(dirA + QStringLiteral("/01 - Xtal.mp3")).absoluteFilePath();
        const auto known = MusicLibrary::byPath(entries);
        CHECK(known.contains(gainPath));
        const TrackEntry g = known.value(gainPath);
        CHECK(g.trackGain.present && qAbs(g.trackGain.value - (-4.50)) < 1e-9);
        CHECK(!g.albumGain.present);          // not tagged -> absent, not 0
    }

    // --- 8. The persisted index round-trips -------------------------------------------------------------
    const QString indexFile = base + QStringLiteral("/musicindex.json");
    {
        CHECK(MusicLibrary::saveIndexFile(indexFile, entries));
        CHECK(QFileInfo::exists(indexFile));
        const QVector<TrackEntry> reloaded = MusicLibrary::loadIndexFile(indexFile);
        CHECK(reloaded.size() == entries.size());
        // The reloaded entries must build a byte-for-byte equivalent index, or a rescan that reuses them all
        // would quietly serve a different library than the scan that wrote them.
        const Index r = MusicLibrary::buildIndex(reloaded);
        CHECK(r.trackCount == idx.trackCount);
        CHECK(r.albumCount == idx.albumCount);
        CHECK(r.artists.size() == idx.artists.size());
        for (int i = 0; i < r.artists.size() && i < idx.artists.size(); ++i)
        {
            CHECK(r.artists[i].key == idx.artists[i].key);
            CHECK(r.artists[i].name == idx.artists[i].name);
            CHECK(r.artists[i].albums.size() == idx.artists[i].albums.size());
        }
        const auto rk = MusicLibrary::byPath(reloaded);
        const TrackEntry g = rk.value(QFileInfo(dirA + QStringLiteral("/01 - Xtal.mp3")).absoluteFilePath());
        CHECK(g.trackGain.present && qAbs(g.trackGain.value - (-4.50)) < 1e-9);
        CHECK(!g.albumGain.present);          // absence round-trips as absence, not as 0 dB
        CHECK(g.mtime != 0 && g.size > 0);

        // Not a file, and a file from a version this build does not know: both load as empty. A wrong-version
        // file re-read as if it were ours would key stale fields onto real paths, which is worse than the
        // full re-tag that an empty load costs.
        CHECK(MusicLibrary::loadIndexFile(base + QStringLiteral("/no-such.json")).isEmpty());
        const QString futureFile = base + QStringLiteral("/future.json");
        CHECK(writeFixture(futureFile, QByteArray("{\"version\":99,\"tracks\":[{\"p\":\"/x.mp3\"}]}")));
        CHECK(MusicLibrary::loadIndexFile(futureFile).isEmpty());
        CHECK(writeFixture(base + QStringLiteral("/junk.json"), QByteArray("not json at all")));
        CHECK(MusicLibrary::loadIndexFile(base + QStringLiteral("/junk.json")).isEmpty());
    }

    // --- 9. INCREMENTAL rescan: an unchanged file is not re-read ---------------------------------------
    // The counter and the observation are both asserted. The counter says the code took the reuse branch;
    // the observation says the file was genuinely not parsed, because its tag block now says something else
    // and the rescan still reports the old value. Either one alone can be satisfied by a broken scan.
    {
        const auto known = MusicLibrary::byPath(MusicLibrary::loadIndexFile(indexFile));
        CHECK(known.size() == kTotalTracks);

        ScanStats s2;
        const QVector<TrackEntry> again = MusicLibrary::scanFolder(root, known, &s2);
        CHECK(again.size() == kTotalTracks);
        CHECK(s2.files == kTotalTracks);
        CHECK(s2.reused == kTotalTracks);
        CHECK(s2.retagged == 0);              // not one file opened
        CHECK(s2.dropped == 0);

        // Rewrite one file's tag AT THE SAME LENGTH ("Xtal" -> "Yodl", four characters either way) and put
        // its mtime back. Same path, same size, same mtime: by the scan's rule this file did not change.
        const QString xtal = QFileInfo(dirA + QStringLiteral("/01 - Xtal.mp3")).absoluteFilePath();
        const qint64 mtimeBefore = QFileInfo(xtal).lastModified().toSecsSinceEpoch();
        const qint64 sizeBefore  = QFileInfo(xtal).size();
        CHECK(writeMp3(xtal, QStringLiteral("Yodl"), QStringLiteral("Aphex Twin"), QString(),
                       QStringLiteral("Selected Ambient Works"), QStringLiteral("1/3"), QString(),
                       QStringLiteral("1992"), QByteArray(), QStringLiteral("-4.50 dB")));
        CHECK(QFileInfo(xtal).size() == sizeBefore);      // or the rewrite, not the cache, is what is tested
        CHECK(setMtime(xtal, mtimeBefore));
        CHECK(QFileInfo(xtal).lastModified().toSecsSinceEpoch() == mtimeBefore);

        ScanStats s3;
        const QVector<TrackEntry> stale = MusicLibrary::scanFolder(root, known, &s3);
        CHECK(s3.retagged == 0);
        CHECK(s3.reused == kTotalTracks);
        CHECK(MusicLibrary::byPath(stale).value(xtal).title == QStringLiteral("Xtal"));  // the OLD title

        // Now advance the mtime. The file is changed, and exactly one file is re-read.
        CHECK(setMtime(xtal, mtimeBefore + 120));
        ScanStats s4;
        const QVector<TrackEntry> fresh = MusicLibrary::scanFolder(root, known, &s4);
        CHECK(s4.retagged == 1);
        CHECK(s4.reused == kTotalTracks - 1);
        CHECK(MusicLibrary::byPath(fresh).value(xtal).title == QStringLiteral("Yodl"));  // the NEW title
        CHECK(MusicLibrary::byPath(fresh).value(xtal).mtime == mtimeBefore + 120);

        // SIZE is the other half of the key, and it earns its place. Rewrite a SECOND file with a title of a
        // DIFFERENT length and put its mtime back: the timestamp says nothing changed and the length says it
        // did. Tag editors that preserve mtime exist, and so do restores from backup — a scan keyed on the
        // timestamp alone serves the old tags for that file forever.
        const QString tha = QFileInfo(dirA + QStringLiteral("/02 - Tha.mp3")).absoluteFilePath();
        const auto known2 = MusicLibrary::byPath(fresh);
        const qint64 thaMtime = QFileInfo(tha).lastModified().toSecsSinceEpoch();
        const qint64 thaSize  = QFileInfo(tha).size();
        CHECK(writeMp3(tha, QStringLiteral("Tha (Extended Mix)"), QStringLiteral("Aphex Twin"), QString(),
                       QStringLiteral("Selected Ambient Works"), QStringLiteral("2/3"), QString(),
                       QStringLiteral("1992")));
        CHECK(QFileInfo(tha).size() != thaSize);          // or the length is not what is being tested
        CHECK(setMtime(tha, thaMtime));
        CHECK(QFileInfo(tha).lastModified().toSecsSinceEpoch() == thaMtime);
        ScanStats s4b;
        const QVector<TrackEntry> resized = MusicLibrary::scanFolder(root, known2, &s4b);
        CHECK(s4b.retagged == 1);
        CHECK(s4b.reused == kTotalTracks - 1);
        CHECK(MusicLibrary::byPath(resized).value(tha).title == QStringLiteral("Tha (Extended Mix)"));

        // A file that is gone is dropped from the scan and from the index built off it.
        const QString loose = QFileInfo(dirG + QStringLiteral("/loose.mp3")).absoluteFilePath();
        CHECK(QFile::remove(loose));
        ScanStats s5;
        const QVector<TrackEntry> after = MusicLibrary::scanFolder(root, MusicLibrary::byPath(fresh), &s5);
        CHECK(after.size() == kTotalTracks - 1);
        CHECK(s5.dropped == 1);
        CHECK(!MusicLibrary::byPath(after).contains(loose));
        const Index ai = MusicLibrary::buildIndex(after);
        CHECK(ai.trackCount == kTotalTracks - 1);
        const Artist* un = ai.artist(QString());
        CHECK(un && un->albums.size() == 1);   // "More Rips" held only that file, so the album goes with it
    }

    // --- 10. Dormant and empty cases are answers, not failures ------------------------------------------
    {
        ScanStats sd;
        CHECK(MusicLibrary::scanFolder(QString(), {}, &sd).isEmpty());
        CHECK(sd.files == 0 && sd.retagged == 0 && sd.dropped == 0);
        CHECK(MusicLibrary::scanFolder(base + QStringLiteral("/no-such-folder")).isEmpty());
        const QString emptyRoot = base + QStringLiteral("/empty");
        QDir().mkpath(emptyRoot);
        CHECK(MusicLibrary::scanFolder(emptyRoot).isEmpty());

        const Index e = MusicLibrary::buildIndex({});
        CHECK(e.isEmpty());
        CHECK(e.trackCount == 0 && e.albumCount == 0);
        CHECK(e.artist(QString()) == nullptr);
        CHECK(e.album(QStringLiteral("anything")) == nullptr);

        // The extension filter is AudioTags' one, not a second copy of it.
        CHECK(MusicLibrary::isAudioFile(QStringLiteral("/m/a.mp3")));
        CHECK(MusicLibrary::isAudioFile(QStringLiteral("/m/a.FLAC")));
        CHECK(!MusicLibrary::isAudioFile(QStringLiteral("/m/cover.jpg")));
        CHECK(!MusicLibrary::isAudioFile(QStringLiteral("/m/film.mkv")));
    }

    // --- 11. MULTI-VALUE ARTISTS AND GENRES IN THE INDEX (issue #196, part 1) ---------------------------
    // Its own root, scanned on its own, so the seventeen-file library above keeps asserting exactly what it
    // always asserted. The question here is different: a track credited to several people has to be findable
    // under EACH of them, while the album it is on stays ONE album under ONE artist.
    {
        const QString mroot = base + QStringLiteral("/multi");

        // THE DEFAULT the whole feature rests on, asserted where nothing else asserts it: the app scans with
        // ONE semicolon and nothing more, because a wrong separator does not miss a split, it shreds a band
        // name into two artists that both look real. An explicitly emptied list must stay distinguishable
        // from a never-configured one, or "split nothing" would be unexpressible.
        CHECK(Settings::musicTagSeparators() == QStringLiteral(";"));
        CHECK(Settings::musicTagSeparatorList() == QStringList{ QStringLiteral(";") });
        Settings::setMusicTagSeparators(QString());
        CHECK(Settings::musicTagSeparatorList().isEmpty());
        Settings::setMusicTagSeparators(QStringLiteral("; / feat."));
        CHECK(Settings::musicTagSeparatorList().size() == 3);
        CHECK(Settings::musicTagSeparatorList().value(2) == QStringLiteral("feat."));

        // A. An album with NO album artist whose every track credits two people, ID3v2.3-style: one string,
        //    a semicolon in it. This is #196's opening complaint — before this, the artist was literally
        //    "Run-D.M.C.; Aerosmith" and neither name reached it.
        const QString mA = mroot + QStringLiteral("/Raising Hell");
        CHECK(writeMp3(mA + QStringLiteral("/01.mp3"), QStringLiteral("Walk This Way"),
                       QStringLiteral("Run-D.M.C.; Aerosmith"), QString(), QStringLiteral("Raising Hell"),
                       QStringLiteral("1/2"), QString(), QStringLiteral("1986")));
        CHECK(writeMp3(mA + QStringLiteral("/02.mp3"), QStringLiteral("My Adidas"),
                       QStringLiteral("Run-D.M.C."), QString(), QStringLiteral("Raising Hell"),
                       QStringLiteral("2/2"), QString(), QStringLiteral("1986")));

        // B. AC/DC, whose name contains a character people use as a separator. With the default list it is
        //    ONE artist with ONE album, and that is the assertion this whole increment is most afraid of.
        const QString mB = mroot + QStringLiteral("/Back in Black");
        CHECK(writeMp3(mB + QStringLiteral("/01.mp3"), QStringLiteral("Hells Bells"),
                       QStringLiteral("AC/DC"), QStringLiteral("AC/DC"), QStringLiteral("Back in Black"),
                       QStringLiteral("1/1"), QString(), QStringLiteral("1980")));

        // C. A COMPILATION: one album artist, three DIFFERENT single-valued track artists. No credit may be
        //    minted here — a single artist that differs from the album's is the "appears on" dimension, not
        //    a multi-value tag, and minting one would give every ordinary library a shelf full of new names.
        const QString mC = mroot + QStringLiteral("/Now Thats What I Call Probing");
        CHECK(writeMp3(mC + QStringLiteral("/a.mp3"), QStringLiteral("One"), QStringLiteral("Soloist A"),
                       QStringLiteral("Various Artists"), QStringLiteral("Probing 96"), QStringLiteral("1/2")));
        CHECK(writeMp3(mC + QStringLiteral("/b.mp3"), QStringLiteral("Two"), QStringLiteral("Soloist B"),
                       QStringLiteral("Various Artists"), QStringLiteral("Probing 96"), QStringLiteral("2/2")));

        // D. A structured Vorbis album: the field REPEATS, and there IS an album artist. Every track credits
        //    a different guest, and the album must stay ONE album under the album artist while each guest
        //    still finds their own track. Genres repeat too, which is the genre half of the same rule.
        const QString mD = mroot + QStringLiteral("/Guest List");
        CHECK(writeFlac(mD + QStringLiteral("/01.flac"), {
            QByteArray("TITLE=Opener"), QByteArray("ARTIST=The Host"), QByteArray("ARTIST=Guest One"),
            QByteArray("ALBUMARTIST=The Host"), QByteArray("ALBUM=Guest List"),
            QByteArray("TRACKNUMBER=1"), QByteArray("DATE=2004"),
            QByteArray("GENRE=Electronic"), QByteArray("GENRE=Ambient") }));
        CHECK(writeFlac(mD + QStringLiteral("/02.flac"), {
            QByteArray("TITLE=Closer"), QByteArray("ARTIST=The Host"), QByteArray("ARTIST=Guest Two"),
            QByteArray("ALBUMARTIST=The Host"), QByteArray("ALBUM=Guest List"),
            QByteArray("TRACKNUMBER=2"), QByteArray("DATE=2004") }));

        const QStringList seps = { QStringLiteral(";") };   // the app's default, passed as the scan does
        ScanStats ms;
        const QVector<TrackEntry> mEntries = MusicLibrary::scanFolder(mroot, {}, &ms, seps);
        CHECK(mEntries.size() == 7);
        const Index m = MusicLibrary::buildIndex(mEntries);
        CHECK(m.trackCount == 7);

        // The albums: Raising Hell, Back in Black, Probing 96, Guest List. FOUR — one per record, not one
        // per credited performer, which is what splitting the album artist would have produced.
        CHECK(m.albumCount == 4);

        // A. The album is filed under the FIRST credited artist, and "Run-D.M.C.; Aerosmith" is not an
        //    artist at all any more.
        const Artist* run = m.artist(keyOf(QStringLiteral("Run-D.M.C.")));
        CHECK(run != nullptr);
        CHECK(m.artist(keyOf(QStringLiteral("Run-D.M.C.; Aerosmith"))) == nullptr);
        if (run)
        {
            CHECK(run->albums.size() == 1);
            CHECK(run->albums.first().tracks.size() == 2);   // BOTH tracks, still one album
            CHECK(run->trackCount == 2);
            CHECK(run->credits.isEmpty());                   // they are the album artist; not a credit
        }
        // ...and Aerosmith, who has no record here at all, finds their one track through the credit index.
        const Artist* aero = m.artist(keyOf(QStringLiteral("Aerosmith")));
        CHECK(aero != nullptr);
        if (aero && run)
        {
            CHECK(aero->albums.isEmpty());                   // no album of their own — they are a guest
            CHECK(aero->trackCount == 0);                    // trackCount is the discography, credits are not
            CHECK(aero->credits.size() == 1);
            CHECK(aero->credits.first().title == QStringLiteral("Walk This Way"));
            // The credit routes back to the album it is on, which is how pressing it plays the right record.
            CHECK(m.album(aero->credits.first().albumKey) != nullptr);
            CHECK(m.album(aero->credits.first().albumKey) == &run->albums.first());
        }

        // B. AC/DC is ONE artist with ONE album. Not two artists, and not two albums.
        const Artist* acdc = m.artist(keyOf(QStringLiteral("AC/DC")));
        CHECK(acdc != nullptr);
        CHECK(m.artist(keyOf(QStringLiteral("AC"))) == nullptr);
        CHECK(m.artist(keyOf(QStringLiteral("DC"))) == nullptr);
        if (acdc)
        {
            CHECK(acdc->name == QStringLiteral("AC/DC"));
            CHECK(acdc->albums.size() == 1);
            CHECK(acdc->credits.isEmpty());
        }

        // C. The compilation mints NO credits: single-valued artists, however much they differ.
        const Artist* va = m.artist(keyOf(QStringLiteral("Various Artists")));
        CHECK(va != nullptr);
        if (va) { CHECK(va->albums.size() == 1); CHECK(va->albums.first().tracks.size() == 2); }
        CHECK(m.artist(keyOf(QStringLiteral("Soloist A"))) == nullptr);
        CHECK(m.artist(keyOf(QStringLiteral("Soloist B"))) == nullptr);

        // D. The structured album stays ONE album under its album artist, with both tracks on it...
        const Artist* host = m.artist(keyOf(QStringLiteral("The Host")));
        CHECK(host != nullptr);
        if (host)
        {
            CHECK(host->albums.size() == 1);
            CHECK(host->albums.first().tracks.size() == 2);
            CHECK(host->credits.isEmpty());
        }
        // ...and each guest reaches their own track and ONLY their own.
        const Artist* g1 = m.artist(keyOf(QStringLiteral("Guest One")));
        const Artist* g2 = m.artist(keyOf(QStringLiteral("Guest Two")));
        CHECK(g1 && g1->albums.isEmpty() && g1->credits.size() == 1);
        CHECK(g2 && g2->albums.isEmpty() && g2->credits.size() == 1);
        if (g1) CHECK(g1->credits.first().title == QStringLiteral("Opener"));
        if (g2) CHECK(g2->credits.first().title == QStringLiteral("Closer"));

        // Genres are multi-valued too, and they ride on the track because nothing browses by genre yet.
        if (host && host->albums.size() == 1 && host->albums.first().tracks.size() == 2)
        {
            const IndexTrack& opener = host->albums.first().tracks.at(0);
            CHECK(opener.genres.size() == 2);
            CHECK(opener.genres.value(0) == QStringLiteral("Electronic"));
            CHECK(opener.genres.value(1) == QStringLiteral("Ambient"));
            CHECK(host->albums.first().tracks.at(1).genres.isEmpty());   // untagged genre yields none
        }

        // The whole artist shelf, so a new bucket cannot appear unnoticed: the four album artists plus the
        // three credit-only guests, and nothing else.
        CHECK(m.artists.size() == 7);

        // THE SETTING IS REAL, in both directions. Scanned with no separators the ad-hoc album falls back to
        // one artist called "Run-D.M.C.; Aerosmith"; scanned with "/" as well, AC/DC is shredded. Neither is
        // the default, and both are what the default is protecting against.
        {
            const Index none = MusicLibrary::buildIndex(MusicLibrary::scanFolder(mroot));
            CHECK(none.artist(keyOf(QStringLiteral("Run-D.M.C.; Aerosmith"))) != nullptr);
            CHECK(none.artist(keyOf(QStringLiteral("Aerosmith"))) == nullptr);
            // The structured Vorbis album is unaffected: it never needed a separator list at all.
            const Artist* h = none.artist(keyOf(QStringLiteral("The Host")));
            CHECK(h && h->albums.size() == 1);
            CHECK(none.artist(keyOf(QStringLiteral("Guest One"))) != nullptr);

            const Index slash = MusicLibrary::buildIndex(
                MusicLibrary::scanFolder(mroot, {}, nullptr, { QStringLiteral("/") }));
            // The RECORD survives even then, because the album artist is never split whatever the list
            // says - but the track credit is shredded, and the shelf grows two bands that do not exist.
            CHECK(slash.artist(keyOf(QStringLiteral("AC/DC"))) != nullptr);
            CHECK(slash.artist(keyOf(QStringLiteral("AC"))) != nullptr);   // the cost of adding "/"
            CHECK(slash.artist(keyOf(QStringLiteral("DC"))) != nullptr);
        }

        // The multi-value lists round-trip through the persisted index, and the SEPARATOR STAMP goes with
        // them — without it a changed setting would never re-tag anything, because an unchanged file is
        // never re-opened.
        {
            const QString mIndexFile = base + QStringLiteral("/multiindex.json");
            CHECK(MusicLibrary::saveIndexFile(mIndexFile, mEntries, seps));
            QString usedSeps;
            const QVector<TrackEntry> reloaded = MusicLibrary::loadIndexFile(mIndexFile, &usedSeps);
            CHECK(usedSeps == QStringLiteral(";"));
            CHECK(reloaded.size() == mEntries.size());
            const Index r = MusicLibrary::buildIndex(reloaded);
            CHECK(r.artists.size() == m.artists.size());
            CHECK(r.albumCount == m.albumCount);
            const Artist* ra = r.artist(keyOf(QStringLiteral("Aerosmith")));
            CHECK(ra && ra->credits.size() == 1);            // a credit survives a reload, not just a scan
            const Artist* rh = r.artist(keyOf(QStringLiteral("The Host")));
            CHECK(rh && rh->albums.size() == 1 && rh->albums.first().tracks.at(0).genres.size() == 2);

            // A pre-#196 index carries no stamp, so it reads as "" — which differs from every configured
            // list and therefore re-tags once. Absence must not be mistaken for agreement.
            QString oldSeps = QStringLiteral("not cleared");
            CHECK(MusicLibrary::loadIndexFile(indexFile, &oldSeps).size() > 0);
            CHECK(oldSeps.isEmpty());
        }
    }

    if (g_fails == 0)
        std::printf("MUSICLIB-OK\n");
    else
        std::printf("MUSICLIB had %d failure(s)\n", g_fails);
    return g_fails == 0 ? 0 : 1;
}
