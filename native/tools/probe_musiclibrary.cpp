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
//  10. THE CLASSICAL VIEW (issue #196, part 2), on a root of its own again: three composers out of five
//      classical files and a pop track; a work split across movements coming back in TRACK order when path
//      order disagrees; two recordings of the same work by the same composer staying TWO rows told apart by
//      their performers; an untagged work borrowing its album's title; two composers on one album not
//      colliding; a conductor credit reaching the track; every work track still pointing at the album it is
//      on; unknown keys answering nullptr; the five fields surviving the persisted index while a pop entry
//      writes none of their keys; and A LIBRARY WITH NO COMPOSER TAG building exactly the index it always
//      built, which is the claim the whole increment rests on.
//
//  11. CUE SHEETS (issue #196, part 3), on a root of its own: a single-file rip stops being one enormous
//      track and becomes its album, one entry on disk expanding into several browse rows whose paths are
//      distinct clips of the same file; a sheet embedded in the audio counts too; a sheet naming a file
//      that is not there claims nothing; a cue over per-track files changes nothing; an untagged rip takes
//      its album, artist, genre and year from the sheet while a TAGGED one does not; editing or deleting
//      the sidecar re-reads exactly that album even though the audio file never moved; and A LIBRARY WITH
//      NO CUE SHEETS scans, groups and persists byte-for-byte as it did, which is the claim that matters.
//
// Prints MUSICLIB-OK on success; any failure prints MUSICLIB-FAIL <cond> (line) and exits non-zero.
//
// Isolation: AppPaths::dataDir() is this process's own scratch dir (issue #42), so the whole fixture library
// is written under it and goes away at exit. Nothing is written beside the exe.
#include "MusicLibrary.h"
#include "AppPaths.h"
#include "Settings.h"   // the DEFAULT separator list is a setting, and section 11 pins it
#include "CueSheet.h"   // #196 part 3: the clip url the cue expansion is asserted against
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

// A FLAC of an arbitrary LENGTH. writeFlac above is fixed at three seconds, which is plenty for a tag but
// useless for a cue: the boundaries of a single-file rip are minutes apart, and a five-minute album is the
// smallest fixture in which 05:02:37 means anything. Only STREAMINFO changes — there are still no audio
// frames, and TagLib reads the duration out of the header, which is the value the cue arithmetic is
// measured against.
static bool writeLongFlac(const QString& path, int seconds, const QList<QByteArray>& comments)
{
    QByteArray flac("fLaC", 4);
    flac.append(flacBlock(0, flacStreamInfo(44100, 2, 16, quint64(44100) * quint64(seconds)), false));
    flac.append(flacBlock(4, flacVorbisComment(comments), true));
    return writeFixture(path, flac);
}

// A .cue sidecar. Written as UTF-8 bytes rather than through QTextStream so the line endings in the literal
// are the line endings on the disk — CRLF is what real sheets carry and is one of the things being pinned.
static bool writeText(const QString& path, const QString& text)
{
    return writeFixture(path, text.toUtf8());
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
            QString usedRules;
            const QVector<TrackEntry> reloaded = MusicLibrary::loadIndexFile(mIndexFile, &usedRules);
            // ONE stamp, holding the separator list AND the tag-reader revision (#196 part 2 folded itself
            // into it rather than adding a second invalidation condition). It is compared against
            // parseStamp() and never against a hand-spelled string, or the two would drift and the compare
            // in MainWindow would start dropping the cache on every scan.
            CHECK(usedRules == MusicLibrary::parseStamp(seps));
            CHECK(!usedRules.isEmpty());
            // The stamp really does distinguish: a DIFFERENT separator list, and the same list under a
            // different reader revision, must both fail the compare that decides whether to re-tag.
            CHECK(MusicLibrary::parseStamp(seps) != MusicLibrary::parseStamp({ QStringLiteral("/") }));
            CHECK(MusicLibrary::parseStamp({}) != QString());
            // AND IT MUST NOT BE ONE WE HAVE ALREADY SHIPPED. Each string here was the stamp of a released
            // reader revision, so re-using one tells an installed library that its cache is current when the
            // reader has since learned to read something new — a change that then appears to do nothing at
            // all until every file in that library is edited by hand. These ARE hand-spelled, deliberately
            // and in the opposite spirit to the compares above: those exist so the stamp cannot drift, this
            // one exists so it cannot drift BACKWARDS. Append to the list when the revision goes up.
            for (const QString& shipped : { QStringLiteral("1 "), QStringLiteral("2 ") })
                CHECK(MusicLibrary::parseStamp({}) != shipped);
            CHECK(reloaded.size() == mEntries.size());
            const Index r = MusicLibrary::buildIndex(reloaded);
            CHECK(r.artists.size() == m.artists.size());
            CHECK(r.albumCount == m.albumCount);
            const Artist* ra = r.artist(keyOf(QStringLiteral("Aerosmith")));
            CHECK(ra && ra->credits.size() == 1);            // a credit survives a reload, not just a scan
            const Artist* rh = r.artist(keyOf(QStringLiteral("The Host")));
            CHECK(rh && rh->albums.size() == 1 && rh->albums.first().tracks.at(0).genres.size() == 2);

            // A pre-stamp index carries no "rules" key at all, so it reads as "" - which differs from
            // every stamp and therefore re-tags exactly once. Absence must not be mistaken for agreement.
            // Written by hand rather than by saveIndexFile, because the whole point is a file this build
            // could not produce; a stamped file used as the "old" one would silently stop testing anything
            // the moment the stamp changed shape.
            {
                const QString legacy = base + QStringLiteral("/legacyindex.json");
                QFile lf(legacy);
                CHECK(lf.open(QIODevice::WriteOnly | QIODevice::Truncate));
                lf.write("{\"version\":1,\"tracks\":[{\"p\":\"/x/a.mp3\",\"m\":1,\"s\":2,"
                         "\"ti\":\"A\",\"ar\":\"Old Band\"}]}");
                lf.close();
                QString oldRules = QStringLiteral("not cleared");
                CHECK(MusicLibrary::loadIndexFile(legacy, &oldRules).size() == 1);
                CHECK(oldRules.isEmpty());
                CHECK(oldRules != MusicLibrary::parseStamp(seps));   // ...so the cache is dropped, once
            }
        }
    }

    // --- 12. THE CLASSICAL VIEW (issue #196, part 2) ----------------------------------------------------
    // Its own root again, so sections 1-11 keep asserting exactly what they always did. The question here
    // is the one the issue asks: can a person find a composer, see what of theirs is on the shelf, tell two
    // recordings of the same piece apart, and reach the movements in order - WITHOUT anything about albums,
    // artists or an ordinary pop track moving an inch.
    {
        const QString croot = base + QStringLiteral("/classical");
        const QStringList seps = { QStringLiteral(";") };

        // A. A WORK SPLIT ACROSS MOVEMENTS. The filenames are chosen so PATH order (a, b, c) is NOT track
        //    order (2, 3, 1): an implementation that just appended what the walk handed it passes every
        //    count below and still lists the Aria third.
        const QString cA = croot + QStringLiteral("/Gould 1955");
        const QList<QByteArray> gouldCommon = {
            QByteArray("ALBUM=Goldberg Variations"), QByteArray("ALBUMARTIST=Glenn Gould"),
            QByteArray("ARTIST=Glenn Gould"), QByteArray("COMPOSER=Johann Sebastian Bach"),
            QByteArray("PERFORMER=Glenn Gould"), QByteArray("WORK=Goldberg Variations, BWV 988"),
            QByteArray("DATE=1955") };
        CHECK(writeFlac(cA + QStringLiteral("/a.flac"), gouldCommon + QList<QByteArray>{
            QByteArray("TITLE=Variatio 1 a 1 Clav."), QByteArray("MOVEMENTNAME=Variatio 1 a 1 Clav."),
            QByteArray("TRACKNUMBER=2") }));
        CHECK(writeFlac(cA + QStringLiteral("/b.flac"), gouldCommon + QList<QByteArray>{
            QByteArray("TITLE=Variatio 2 a 1 Clav."), QByteArray("MOVEMENTNAME=Variatio 2 a 1 Clav."),
            QByteArray("TRACKNUMBER=3") }));
        CHECK(writeFlac(cA + QStringLiteral("/c.flac"), gouldCommon + QList<QByteArray>{
            QByteArray("TITLE=Aria"), QByteArray("MOVEMENTNAME=Aria"),
            QByteArray("TRACKNUMBER=1") }));

        // B. THE SAME WORK BY THE SAME COMPOSER, A DIFFERENT PERFORMER. Two recordings of one piece are two
        //    rows, or a shelf of Bach is a shelf of one row per title with everybody's performances heaped
        //    into it, ordered by a track number that means something different in each.
        const QString cB = croot + QStringLiteral("/Bezuidenhout");
        CHECK(writeFlac(cB + QStringLiteral("/01.flac"), {
            QByteArray("TITLE=Aria"), QByteArray("ALBUM=Goldberg Variations (Harpsichord)"),
            QByteArray("ALBUMARTIST=Kristian Bezuidenhout"), QByteArray("ARTIST=Kristian Bezuidenhout"),
            QByteArray("COMPOSER=Johann Sebastian Bach"), QByteArray("PERFORMER=Kristian Bezuidenhout"),
            QByteArray("WORK=Goldberg Variations, BWV 988"), QByteArray("MOVEMENTNAME=Aria"),
            QByteArray("TRACKNUMBER=1"), QByteArray("DATE=2020") }));

        // C. A CONDUCTOR CREDIT, and a SECOND composer, on a disc whose files carry no WORK tag at all -
        //    so the work row has to borrow the album's title, which is the "works/albums" half of the rule.
        //    Two composers on ONE album is also the case that would collide if the work key did not carry
        //    the composer.
        const QString cC = croot + QStringLiteral("/Requiem");
        CHECK(writeFlac(cC + QStringLiteral("/01.flac"), {
            QByteArray("TITLE=Introitus"), QByteArray("ALBUM=Requiem"),
            QByteArray("ALBUMARTIST=Monteverdi Choir"), QByteArray("ARTIST=Monteverdi Choir"),
            QByteArray("COMPOSER=Wolfgang Amadeus Mozart"),
            QByteArray("CONDUCTOR=John Eliot Gardiner"),
            QByteArray("PERFORMER=Monteverdi Choir"), QByteArray("TRACKNUMBER=1"), QByteArray("DATE=1986") }));
        CHECK(writeFlac(cC + QStringLiteral("/02.flac"), {
            QByteArray("TITLE=Lacrimosa"), QByteArray("ALBUM=Requiem"),
            QByteArray("ALBUMARTIST=Monteverdi Choir"), QByteArray("ARTIST=Monteverdi Choir"),
            QByteArray("COMPOSER=Wolfgang Amadeus Mozart; Franz Xaver Sussmayr"),   // ad-hoc split, two names
            QByteArray("CONDUCTOR=John Eliot Gardiner"),
            QByteArray("PERFORMER=Monteverdi Choir"), QByteArray("TRACKNUMBER=2"), QByteArray("DATE=1986") }));

        // D. THE POP TRACK. No composer, no conductor, no work - the file most of this app's users have in
        //    every folder they own. It shares the root so that it is scanned by the same pass, and it must
        //    come out of the index exactly as it did before any of this existed.
        const QString cD = croot + QStringLiteral("/Dummy");
        CHECK(writeMp3(cD + QStringLiteral("/01.mp3"), QStringLiteral("Glory Box"),
                       QStringLiteral("Portishead"), QStringLiteral("Portishead"), QStringLiteral("Dummy"),
                       QStringLiteral("1/1"), QString(), QStringLiteral("1994")));

        const QVector<TrackEntry> cEntries = MusicLibrary::scanFolder(croot, {}, nullptr, seps);
        CHECK(cEntries.size() == 7);
        const Index ci = MusicLibrary::buildIndex(cEntries);

        // 12a. THE ARTIST/ALBUM SIDE IS UNTOUCHED. Four album artists, five albums, seven tracks - exactly
        //      what this root would have produced before the composer pass existed. If a composer bucket
        //      ever starts creating artists or albums, this is the assertion that says so.
        CHECK(ci.trackCount == 7);
        CHECK(ci.albumCount == 4);
        CHECK(ci.artists.size() == 4);
        CHECK(ci.artist(keyOf(QStringLiteral("Portishead"))) != nullptr);
        CHECK(ci.artist(keyOf(QStringLiteral("Glenn Gould"))) != nullptr);
        CHECK(ci.artist(keyOf(QStringLiteral("Johann Sebastian Bach"))) == nullptr);   // NOT an artist

        // 12b. THREE COMPOSERS, alphabetically, and the pop track is in none of them.
        CHECK(ci.composers.size() == 3);
        CHECK(ci.composers.at(0).name == QStringLiteral("Franz Xaver Sussmayr"));
        CHECK(ci.composers.at(1).name == QStringLiteral("Johann Sebastian Bach"));
        CHECK(ci.composers.at(2).name == QStringLiteral("Wolfgang Amadeus Mozart"));

        // 12c. BACH: two recordings of one work, each its own row, each with its own performer. The count of
        //      TRACKS is over both.
        const MusicLibrary::Composer* bach = ci.composer(keyOf(QStringLiteral("Johann Sebastian Bach")));
        CHECK(bach != nullptr);
        CHECK(bach && bach->works.size() == 2);
        CHECK(bach && bach->trackCount == 4);
        if (bach && bach->works.size() == 2)
        {
            // Both rows carry the WORK title, so they sort together and are told apart by who is playing.
            CHECK(bach->works.at(0).title == QStringLiteral("Goldberg Variations, BWV 988"));
            CHECK(bach->works.at(1).title == QStringLiteral("Goldberg Variations, BWV 988"));
            CHECK(bach->works.at(0).fromWork && bach->works.at(1).fromWork);
            CHECK(bach->works.at(0).key != bach->works.at(1).key);
            QStringList players;
            for (const MusicLibrary::ComposerWork& w : bach->works) players << w.performers.join(QStringLiteral(","));
            CHECK(players.contains(QStringLiteral("Glenn Gould")));
            CHECK(players.contains(QStringLiteral("Kristian Bezuidenhout")));

            // THE MOVEMENTS ARE IN TRACK ORDER, not the a/b/c path order the walk found them in.
            const MusicLibrary::ComposerWork* gould = nullptr;
            for (const MusicLibrary::ComposerWork& w : bach->works)
                if (w.performers.value(0) == QStringLiteral("Glenn Gould")) gould = &w;
            CHECK(gould != nullptr);
            if (gould)
            {
                CHECK(gould->tracks.size() == 3);
                CHECK(gould->tracks.at(0).movement == QStringLiteral("Aria"));
                CHECK(gould->tracks.at(1).movement == QStringLiteral("Variatio 1 a 1 Clav."));
                CHECK(gould->tracks.at(2).movement == QStringLiteral("Variatio 2 a 1 Clav."));
                CHECK(gould->tracks.at(0).track == 1);
                // Every track still points at the album it is ON, so pressing one plays that record.
                const Album* on = ci.album(gould->albumKey);
                CHECK(on && on->title == QStringLiteral("Goldberg Variations"));
                for (const MusicLibrary::IndexTrack& t : gould->tracks) CHECK(t.albumKey == gould->albumKey);
                CHECK(gould->durationSec == 9);       // three 3-second FLACs, hand-computed
            }
        }

        // 12d. MOZART: an UNTAGGED work borrows the album's title, the conductor rides on the track, and
        //      the ad-hoc split really did mint the second composer off one string.
        const MusicLibrary::Composer* moz = ci.composer(keyOf(QStringLiteral("Wolfgang Amadeus Mozart")));
        CHECK(moz && moz->works.size() == 1);
        CHECK(moz && moz->trackCount == 2);
        if (moz && moz->works.size() == 1)
        {
            const MusicLibrary::ComposerWork& w = moz->works.first();
            CHECK(!w.fromWork);                                       // no WORK tag anywhere on that disc
            CHECK(w.title == QStringLiteral("Requiem"));              // borrowed from the album
            CHECK(w.tracks.size() == 2);
            CHECK(w.tracks.at(0).conductors == QStringList({ QStringLiteral("John Eliot Gardiner") }));
            // Performers first, then the conductor - both are "who is playing" and neither is the album.
            CHECK(w.performers.contains(QStringLiteral("Monteverdi Choir")));
            CHECK(w.performers.contains(QStringLiteral("John Eliot Gardiner")));
        }
        const MusicLibrary::Composer* sus = ci.composer(keyOf(QStringLiteral("Franz Xaver Sussmayr")));
        CHECK(sus && sus->trackCount == 1);                           // the one movement he finished
        CHECK(sus && sus->works.size() == 1);
        // TWO composers on ONE album are two work rows, not one row that collided.
        if (sus && moz && !sus->works.isEmpty() && !moz->works.isEmpty())
            CHECK(sus->works.first().key != moz->works.first().key);

        // 12e. LOOKUPS, which is how a browse route gets back here. A stale/unknown key is a nullptr and
        //      never a crash, exactly as artist()/album() behave.
        CHECK(ci.composer(QStringLiteral("no-such-composer")) == nullptr);
        CHECK(ci.work(QStringLiteral("no-such-work")) == nullptr);
        if (bach && !bach->works.isEmpty())
            CHECK(ci.work(bach->works.first().key) != nullptr);

        // 12f. THE POP LIBRARY IS UNCHANGED, and this is the assertion the increment lives or dies by. The
        //      SAME root minus the classical folders builds an index with NO composers at all, and its one
        //      album is the album it always was.
        {
            const QString proot = base + QStringLiteral("/poponly");
            CHECK(writeMp3(proot + QStringLiteral("/Dummy/01.mp3"), QStringLiteral("Glory Box"),
                           QStringLiteral("Portishead"), QStringLiteral("Portishead"),
                           QStringLiteral("Dummy"), QStringLiteral("1/1"), QString(),
                           QStringLiteral("1994")));
            const Index pi = MusicLibrary::buildIndex(MusicLibrary::scanFolder(proot, {}, nullptr, seps));
            CHECK(pi.composers.isEmpty());
            CHECK(pi.artists.size() == 1);
            CHECK(pi.trackCount == 1 && pi.albumCount == 1);
            const Artist* pa = pi.artist(keyOf(QStringLiteral("Portishead")));
            CHECK(pa && pa->albums.size() == 1 && pa->albums.first().tracks.size() == 1);
            const MusicLibrary::IndexTrack& pt = pa->albums.first().tracks.first();
            CHECK(pt.composers.isEmpty() && pt.conductors.isEmpty() && pt.performers.isEmpty());
            CHECK(pt.work.isEmpty() && pt.movement.isEmpty());
            CHECK(pt.title == QStringLiteral("Glory Box"));
        }

        // 12g. PERSISTENCE. The five fields round-trip, an entry that carries none of them writes none of
        //      the keys (a pop library's index does not grow by a byte), and a reloaded index rebuilds the
        //      same classical view - because the browse reads a RELOADED index far more often than a fresh
        //      scan, and a field that survived the scan and not the file would be invisible until a re-tag.
        {
            const QString cIndexFile = base + QStringLiteral("/classicalindex.json");
            CHECK(MusicLibrary::saveIndexFile(cIndexFile, cEntries, seps));
            const QVector<TrackEntry> reloaded = MusicLibrary::loadIndexFile(cIndexFile);
            CHECK(reloaded.size() == cEntries.size());
            const Index ri = MusicLibrary::buildIndex(reloaded);
            CHECK(ri.composers.size() == 3);
            const MusicLibrary::Composer* rb = ri.composer(keyOf(QStringLiteral("Johann Sebastian Bach")));
            CHECK(rb && rb->works.size() == 2 && rb->trackCount == 4);
            const MusicLibrary::Composer* rm = ri.composer(keyOf(QStringLiteral("Wolfgang Amadeus Mozart")));
            CHECK(rm && rm->works.size() == 1);
            CHECK(rm && !rm->works.isEmpty() && rm->works.first().tracks.at(0).conductors.size() == 1);
            for (const TrackEntry& e : reloaded)
                if (e.title == QStringLiteral("Aria") && e.album == QStringLiteral("Goldberg Variations"))
                {
                    CHECK(e.work == QStringLiteral("Goldberg Variations, BWV 988"));
                    CHECK(e.movement == QStringLiteral("Aria"));
                    CHECK(e.composers == QStringList({ QStringLiteral("Johann Sebastian Bach") }));
                    CHECK(e.performers == QStringList({ QStringLiteral("Glenn Gould") }));
                }
            // The POP entry writes none of the five keys. Read off the raw JSON, because "the round-trip
            // agreed" would pass just as happily with five empty strings stored per track.
            QFile jf(cIndexFile);
            CHECK(jf.open(QIODevice::ReadOnly));
            const QByteArray raw = jf.readAll();
            jf.close();
            CHECK(raw.contains("Glory Box"));                       // the pop entry really is in there
            CHECK(raw.count("\"cm\":") == 6);                        // one per classical file, and no more
            CHECK(raw.count("\"cd\":") == 2);                        // the two conductor-tagged Requiem files
        }
    }

    // --- 13. CUE SHEETS (issue #196, part 3) ------------------------------------------------------------
    // Its own root again, so every section above keeps asserting exactly what it always did. The question
    // here is the one the issue asks: does a single-file rip stop being one enormous "track" — and does a
    // library with no cue sheets in it stay untouched, which is the acceptance test that matters most
    // because almost nobody has these.
    {
        const QString qroot = base + QStringLiteral("/cue");

        // A. THE SINGLE-FILE RIP. One five-minute flac, one sidecar naming three tracks. The flac carries
        //    ALBUM/ALBUMARTIST tags, so the GROUPING comes from the file exactly as it always has and only
        //    the TRACKS come from the sheet.
        const QString qA   = qroot + QStringLiteral("/Portishead - Dummy");
        const QString rip  = qA + QStringLiteral("/Dummy.flac");
        const QString cue  = qA + QStringLiteral("/Dummy.cue");
        CHECK(writeLongFlac(rip, 300, { QByteArray("ALBUM=Dummy"), QByteArray("ALBUMARTIST=Portishead"),
                                        QByteArray("ARTIST=Portishead"), QByteArray("DATE=1994") }));
        CHECK(writeText(cue, QStringLiteral(
            "PERFORMER \"Portishead\"\r\n"
            // DELIBERATELY NOT the album tag: the sheet fills in what a file did not say and never overrules
            // what it did, so this spelling must not reach the index.
            "TITLE \"Dummy (EAC rip)\"\r\n"
            "FILE \"Dummy.wav\" WAVE\r\n"          // the .wav it was transcoded FROM: the commonest mismatch
            "  TRACK 01 AUDIO\r\n    TITLE \"Mysterons\"\r\n    INDEX 01 00:00:00\r\n"
            "  TRACK 02 AUDIO\r\n    TITLE \"Sour Times\"\r\n    PERFORMER \"Beth Gibbons\"\r\n"
            "    INDEX 00 05:00:00\r\n    INDEX 01 05:02:37\r\n"
            "  TRACK 03 AUDIO\r\n    TITLE \"Strangers\"\r\n    INDEX 01 09:11:00\r\n")));

        // B. A CUE NAMING A FILE THAT IS NOT THERE. It must describe NOTHING — not the neighbour it happens
        //    to share a folder with, which would give that file somebody else's track list.
        const QString qB = qroot + QStringLiteral("/Ghost");
        CHECK(writeLongFlac(qB + QStringLiteral("/Real.flac"), 200,
                            { QByteArray("ALBUM=Real"), QByteArray("ARTIST=Somebody") }));
        CHECK(writeText(qB + QStringLiteral("/Ghost.cue"), QStringLiteral(
            "FILE \"Ghost.flac\" WAVE\nTRACK 01 AUDIO\nINDEX 01 00:00:00\n"
            "TRACK 02 AUDIO\nINDEX 01 01:00:00\n")));

        // C. AN EMBEDDED CUESHEET TAG AND NO SIDECAR — how EAC-descended FLAC rips carry the same thing.
        const QString qC = qroot + QStringLiteral("/Embedded");
        CHECK(writeLongFlac(qC + QStringLiteral("/Embedded.flac"), 120, {
            QByteArray("ALBUM=Inside Job"), QByteArray("ARTIST=The Tag"),
            QByteArray("CUESHEET=FILE \"Embedded.flac\" WAVE\n"
                       "  TRACK 01 AUDIO\n    TITLE \"First\"\n    INDEX 01 00:00:00\n"
                       "  TRACK 02 AUDIO\n    TITLE \"Second\"\n    INDEX 01 00:40:00\n") }));

        // D. AN UNTAGGED RIP whose whole metadata is the sheet. Without the sheet standing in, this is
        //    "Unknown Artist" holding an album named after its folder.
        const QString qD = qroot + QStringLiteral("/Untagged");
        CHECK(writeLongFlac(qD + QStringLiteral("/disc.flac"), 180, {}));
        CHECK(writeText(qD + QStringLiteral("/disc.cue"), QStringLiteral(
            "REM GENRE \"Trip Hop\"\nREM DATE 1998\n"
            "PERFORMER \"Massive Attack\"\nTITLE \"Mezzanine\"\n"
            "FILE \"disc.flac\" WAVE\n"
            "TRACK 01 AUDIO\n  TITLE \"Angel\"\n  INDEX 01 00:00:00\n"
            "TRACK 02 AUDIO\n  TITLE \"Risingson\"\n  INDEX 01 01:00:00\n")));

        // E. A CUE OVER PER-TRACK FILES. That album is already a folder of files; nothing may change.
        const QString qE = qroot + QStringLiteral("/PerTrack");
        CHECK(writeLongFlac(qE + QStringLiteral("/01.flac"), 60,
                            { QByteArray("ALBUM=Split"), QByteArray("ARTIST=Split Artist"),
                              QByteArray("TITLE=One"), QByteArray("TRACKNUMBER=1") }));
        CHECK(writeLongFlac(qE + QStringLiteral("/02.flac"), 60,
                            { QByteArray("ALBUM=Split"), QByteArray("ARTIST=Split Artist"),
                              QByteArray("TITLE=Two"), QByteArray("TRACKNUMBER=2") }));
        CHECK(writeText(qE + QStringLiteral("/Split.cue"), QStringLiteral(
            "FILE \"01.flac\" WAVE\nTRACK 01 AUDIO\nINDEX 01 00:00:00\n"
            "FILE \"02.flac\" WAVE\nTRACK 02 AUDIO\nINDEX 01 00:00:00\n")));

        // F. BOTH AT ONCE: a rip carrying an embedded CUESHEET *and* a sidecar beside it. The SIDECAR wins,
        //    because the tag is baked into the file and the sidecar is the thing a person can fix — so the
        //    one they edited has to be the one that counts.
        const QString qF = qroot + QStringLiteral("/Both");
        CHECK(writeLongFlac(qF + QStringLiteral("/Both.flac"), 240, {
            QByteArray("ALBUM=Both Ways"), QByteArray("ARTIST=Two Sheets"),
            QByteArray("CUESHEET=FILE \"Both.flac\" WAVE\n"
                       "  TRACK 01 AUDIO\n    TITLE \"Stale One\"\n    INDEX 01 00:00:00\n"
                       "  TRACK 02 AUDIO\n    TITLE \"Stale Two\"\n    INDEX 01 01:00:00\n") }));
        CHECK(writeText(qF + QStringLiteral("/Both.cue"), QStringLiteral(
            "FILE \"Both.flac\" WAVE\n"
            "TRACK 01 AUDIO\n  TITLE \"Fixed One\"\n  INDEX 01 00:00:00\n"
            "TRACK 02 AUDIO\n  TITLE \"Fixed Two\"\n  INDEX 01 00:50:00\n"
            "TRACK 03 AUDIO\n  TITLE \"Fixed Three\"\n  INDEX 01 02:00:00\n")));

        MusicLibrary::ScanStats qs;
        QVector<TrackEntry> qe = MusicLibrary::scanFolder(qroot, {}, &qs);
        CHECK(qs.files == 7);          // seven flacs; the five .cue files are not audio and are not tracks
        CHECK(qe.size() == 7);
        const QHash<QString, TrackEntry> byq = MusicLibrary::byPath(qe);

        // --- A: one FILE, one entry, three cue tracks ---
        {
            const TrackEntry& e = byq.value(rip);
            CHECK(e.path == rip);                       // still ONE entry for ONE file: file identity is intact
            CHECK(e.cuePath == cue);
            CHECK(e.cueMtime != 0 && e.cueSize > 0);
            CHECK(e.cueTracks.size() == 3);
            CHECK(e.cueTracks.at(0).title == QStringLiteral("Mysterons"));
            CHECK(e.cueTracks.at(0).startMs == 0);
            CHECK(e.cueTracks.at(0).endMs == 302493);   // the NEXT track INDEX 01 — not its 05:00 pregap
            CHECK(e.cueTracks.at(1).artist == QStringLiteral("Beth Gibbons"));
            CHECK(e.cueTracks.at(2).endMs == -1);       // the last track runs to the end of the file
            CHECK(e.album == QStringLiteral("Dummy"));  // from the TAG; the sheet only fills in what is missing
        }

        // --- B/C/E: what the cue does and does not claim ---
        CHECK(byq.value(qB + QStringLiteral("/Real.flac")).cueTracks.isEmpty());
        CHECK(byq.value(qB + QStringLiteral("/Real.flac")).cuePath.isEmpty());
        CHECK(byq.value(qC + QStringLiteral("/Embedded.flac")).cueTracks.size() == 2);
        CHECK(byq.value(qC + QStringLiteral("/Embedded.flac")).cuePath.isEmpty());   // it was in the file
        CHECK(byq.value(qE + QStringLiteral("/01.flac")).cueTracks.isEmpty());
        CHECK(byq.value(qE + QStringLiteral("/02.flac")).cueTracks.isEmpty());
        {
            // F: the sidecar wins over the embedded sheet — three tracks, and the SIDECAR titles.
            const TrackEntry& e = byq.value(qF + QStringLiteral("/Both.flac"));
            CHECK(e.cueTracks.size() == 3);
            CHECK(e.cueTracks.at(0).title == QStringLiteral("Fixed One"));
            CHECK(e.cueTracks.at(1).title == QStringLiteral("Fixed Two"));
        }

        // --- D: the sheet standing in for a file that says nothing ---
        {
            const TrackEntry& e = byq.value(qD + QStringLiteral("/disc.flac"));
            CHECK(e.untagged);                                        // the FILE really carries nothing
            CHECK(e.album == QStringLiteral("Mezzanine"));
            CHECK(e.albumArtist == QStringLiteral("Massive Attack"));
            CHECK(e.artists == QStringList{ QStringLiteral("Massive Attack") });
            CHECK(e.genre == QStringLiteral("Trip Hop"));
            CHECK(e.year == 1998);
        }

        // --- The index: an album with its real tracks ---
        const Index qi = MusicLibrary::buildIndex(qe);
        const Album* dummy = qi.album(MusicLibrary::albumKeyFor(byq.value(rip)));
        CHECK(dummy != nullptr);
        if (dummy)
        {
            CHECK(dummy->tracks.size() == 3);                   // NOT one seventy-minute item
            CHECK(dummy->tracks.at(0).title == QStringLiteral("Mysterons"));
            CHECK(dummy->tracks.at(1).title == QStringLiteral("Sour Times"));
            CHECK(dummy->tracks.at(2).title == QStringLiteral("Strangers"));
            CHECK(dummy->tracks.at(1).artist == QStringLiteral("Beth Gibbons"));
            CHECK(dummy->tracks.at(0).track == 1 && dummy->tracks.at(2).track == 3);
            // THE DURATIONS ARE THE TRACKS, and they are the boundary arithmetic in seconds: 0 -> 302.493
            // is 302 s, 302.493 -> 551 is 249 s (248.507 rounded), and the last is what is left of a 300 s
            // file from 551 s in, which is nothing at all. A cue whose times overrun its file is a real
            // thing and must not produce a negative length.
            CHECK(dummy->tracks.at(0).durationSec == 302);
            CHECK(dummy->tracks.at(1).durationSec == 249);
            CHECK(dummy->tracks.at(2).durationSec == 0);
            // The ALBUM duration stays the FILE - it is already the sum of its tracks.
            CHECK(dummy->durationSec == 300);

            // EVERY TRACK IS A DISTINCT PLAYABLE HANDLE OVER THE SAME FILE. That is what lets the ordinary
            // queue hold all three and start at the second, and it is why the file is never split.
            CHECK(dummy->tracks.at(0).sourcePath == rip);
            CHECK(dummy->tracks.at(1).sourcePath == rip);
            CHECK(dummy->tracks.at(0).path != dummy->tracks.at(1).path);
            CHECK(dummy->tracks.at(1).path != dummy->tracks.at(2).path);
            CHECK(dummy->tracks.at(1).path == CueSheet::mpvClipUrl(rip, 302493, 551000));
            CHECK(dummy->tracks.at(1).path.startsWith(QStringLiteral("edl://")));
            CHECK(dummy->tracks.at(1).path.contains(rip));
            CHECK(dummy->tracks.at(2).path.endsWith(QStringLiteral(",551.000;")));   // no faked length
        }

        // The counts follow the ROWS, so a browse subtitle describes the record rather than the disk.
        const Artist* pa = qi.artist(keyOf(QStringLiteral("Portishead")));
        CHECK(pa != nullptr);
        if (pa) CHECK(pa->trackCount == 3);
        // 3 (Dummy) + 1 (Real) + 2 (Embedded) + 2 (Untagged) + 2 (PerTrack) + 3 (Both) == 13 rows, from 7
        // files. That difference IS the feature: four of those seven files are one track each, and three
        // of them are albums.
        CHECK(qi.trackCount == 13);
        CHECK(qi.albumCount == 6);

        // The untagged rip is a NAMED album under a NAMED artist rather than the unknown bucket.
        const Artist* ma = qi.artist(keyOf(QStringLiteral("Massive Attack")));
        CHECK(ma != nullptr);
        if (ma && !ma->albums.isEmpty())
        {
            CHECK(ma->albums.first().title == QStringLiteral("Mezzanine"));
            CHECK(!ma->albums.first().titleFromFolder);
            CHECK(ma->albums.first().tracks.size() == 2);
            CHECK(ma->albums.first().tracks.at(0).title == QStringLiteral("Angel"));
        }

        // --- The incremental rescan, with the sidecar in the key -------------------------------------
        MusicLibrary::ScanStats qs2;
        QVector<TrackEntry> qe2 = MusicLibrary::scanFolder(qroot, MusicLibrary::byPath(qe), &qs2);
        CHECK(qs2.reused == 7);
        CHECK(qs2.retagged == 0);          // nothing moved, so nothing is opened — the steady state

        // NOW EDIT ONLY THE .CUE. The audio file mtime and size are untouched, so a scan that watched only
        // the audio would show yesterday track list forever. This is the whole reason the sidecar identity
        // is part of the reuse decision.
        CHECK(writeText(cue, QStringLiteral(
            "PERFORMER \"Portishead\"\r\nTITLE \"Dummy (EAC rip)\"\r\nFILE \"Dummy.wav\" WAVE\r\n"
            "  TRACK 01 AUDIO\r\n    TITLE \"Mysterons (remaster)\"\r\n    INDEX 01 00:00:00\r\n"
            "  TRACK 02 AUDIO\r\n    TITLE \"Sour Times\"\r\n    INDEX 01 05:02:37\r\n"
            "  TRACK 03 AUDIO\r\n    TITLE \"Strangers\"\r\n    INDEX 01 09:11:00\r\n"
            "  TRACK 04 AUDIO\r\n    TITLE \"It Could Be Sweet\"\r\n    INDEX 01 12:00:00\r\n")));
        CHECK(setMtime(cue, QFileInfo(cue).lastModified().toSecsSinceEpoch() + 120));
        MusicLibrary::ScanStats qs3;
        QVector<TrackEntry> qe3 = MusicLibrary::scanFolder(qroot, MusicLibrary::byPath(qe2), &qs3);
        CHECK(qs3.retagged == 1);          // exactly the one album whose sheet changed
        CHECK(qs3.reused == 6);
        CHECK(MusicLibrary::byPath(qe3).value(rip).cueTracks.size() == 4);
        CHECK(MusicLibrary::byPath(qe3).value(rip).cueTracks.at(0).title
              == QStringLiteral("Mysterons (remaster)"));

        // --- Persistence -------------------------------------------------------------------------------
        const QString qIndexFile = base + QStringLiteral("/cueindex.json");
        CHECK(MusicLibrary::saveIndexFile(qIndexFile, qe3));
        QString qRules;
        const QVector<TrackEntry> qBack = MusicLibrary::loadIndexFile(qIndexFile, &qRules);
        CHECK(qRules == MusicLibrary::parseStamp({}));
        CHECK(qBack.size() == qe3.size());
        {
            const TrackEntry& e = MusicLibrary::byPath(qBack).value(rip);
            CHECK(e.cueTracks.size() == 4);
            CHECK(e.cueTracks.at(0).title == QStringLiteral("Mysterons (remaster)"));
            CHECK(e.cueTracks.at(1).startMs == 302493);
            CHECK(e.cueTracks.at(1).endMs == 551000);
            CHECK(e.cueTracks.at(3).endMs == -1);       // the open-ended last track survives the round trip
            CHECK(e.cuePath == cue && e.cueMtime != 0 && e.cueSize > 0);
            // A round-tripped index builds the same album, which is what a cold start actually does.
            const Album* rebuilt = MusicLibrary::buildIndex(qBack).album(MusicLibrary::albumKeyFor(e));
            CHECK(rebuilt != nullptr);
            if (rebuilt) CHECK(rebuilt->tracks.size() == 4);
        }

        // DELETING the sheet puts the album back to being one file, and re-reads exactly one entry.
        CHECK(QFile::remove(cue));
        MusicLibrary::ScanStats qs4;
        QVector<TrackEntry> qe4 = MusicLibrary::scanFolder(qroot, MusicLibrary::byPath(qe3), &qs4);
        CHECK(qs4.retagged == 1);
        CHECK(MusicLibrary::byPath(qe4).value(rip).cueTracks.isEmpty());
        CHECK(MusicLibrary::byPath(qe4).value(rip).cuePath.isEmpty());

        // --- THE ACCEPTANCE TEST THAT MATTERS MOST: a library with no cue sheets is untouched ------------
        // Re-scanned and re-indexed here rather than trusted from section 1, because "unaffected" has to
        // mean unaffected AFTER this code exists. Same counts, same grouping, and an index file that does
        // not carry a single cue key — the entry shape for an ordinary library is byte-for-byte what it was.
        {
            MusicLibrary::ScanStats ns;
            const QVector<TrackEntry> ne = MusicLibrary::scanFolder(root, {}, &ns);
            for (const TrackEntry& e : ne)
            {
                CHECK(e.cueTracks.isEmpty());
                CHECK(e.cuePath.isEmpty());
                CHECK(e.cueMtime == 0 && e.cueSize == 0);
            }
            const QString nFile = base + QStringLiteral("/nocueindex.json");
            CHECK(MusicLibrary::saveIndexFile(nFile, ne));
            QFile nf(nFile);
            CHECK(nf.open(QIODevice::ReadOnly));
            const QByteArray nRaw = nf.readAll();
            nf.close();
            CHECK(!nRaw.contains("\"cue\":"));
            CHECK(!nRaw.contains("\"cp\":"));
            CHECK(!nRaw.contains("\"cmt\":"));
            CHECK(!nRaw.contains("\"csz\":"));
            // And the incremental path is still the incremental path: nothing is re-opened, and no file
            // gained a sidecar lookup that could make it look changed.
            MusicLibrary::ScanStats ns2;
            MusicLibrary::scanFolder(root, MusicLibrary::byPath(ne), &ns2);
            CHECK(ns2.retagged == 0);
            CHECK(ns2.reused == ns.files);
        }
    }

    if (g_fails == 0)
        std::printf("MUSICLIB-OK\n");
    else
        std::printf("MUSICLIB had %d failure(s)\n", g_fails);
    return g_fails == 0 ? 0 : 1;
}
