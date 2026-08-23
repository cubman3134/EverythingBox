// Headless check of the CUE SHEET parser (src/media/CueSheet — issue #196, part 3): the layer that turns a
// single-file rip's `.cue` sidecar into the album's real track list, and turns one of those tracks into
// something mpv can play without the file ever being split.
//
// Everything here is TEXT IN, VALUES OUT. No audio, no player, no window, and only one section touches a
// disk (load(), which is the one function that is allowed to). That is the whole reason the parser is its
// own file: the judgement in this feature is in the parsing and the arithmetic, and both have to be pinnable
// without an album to play.
//
// What it pins, and why each one is here rather than being obvious:
//   1. THE TIMESTAMP. MM:SS:FF where FF is FRAMES AT 75 PER SECOND. This is the one number that goes wrong
//      silently — read as hundredths or as milliseconds it still yields a playable album whose every track
//      starts slightly off, and nothing errors. The values below are hand-computed from 1/75 s and the
//      mutation matrix breaks the 75 specifically.
//   2. THE SHAPES REAL SHEETS COME IN: quoted and unquoted values, CRLF and lone-CR line endings, REM lines,
//      indentation, ISRC/FLAGS/CATALOG noise, a FILE line with and without its format word, and a title
//      whose closing quote is missing.
//   3. THE ENCODINGS. A .cue declares none, so a UTF-8 sheet and a Windows-1252 sheet are told apart by
//      trying strict UTF-8 first — and a BOM, when there is one, answers outright.
//   4. INDEX 00 IS READ AND MOVES NOTHING. A pregap belongs to the tail of the outgoing track, because that
//      is where mpv's own cue demuxer puts it and two answers about the same album would differ by seconds.
//   5. REFUSAL. A sheet with an unplaceable track, an out-of-range frame count, backwards or duplicated
//      timestamps, or no FILE at all is INVALID — and an invalid sheet yields no segments, so the library
//      shows the one big file it already had rather than an album with a track in the wrong place.
//   6. "THIS CUE CHANGES NOTHING" is one function's answer. A multi-FILE sheet (an album that is already a
//      folder of files), a one-track sheet, and an invalid one all produce an empty segment list, so the
//      scanner has no second opinion to hold.
//   7. THE CLIP URL. mpv's EDL: length-prefixed so a comma in a path cannot split it, byte-counted rather
//      than character-counted, and with NO length on the last track — a faked one is reported as the
//      duration, so the progress bar would lie for the whole of the closing song.
//
// Prints CUE-OK on success; any failure prints CUE-FAIL <cond> (line) and exits non-zero.
#include "CueSheet.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QString>
#include <cstdio>

static int g_fails = 0;
#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (!(cond)) { std::printf("CUE-FAIL %s (%d)\n", #cond, __LINE__); ++g_fails; } \
    } while (0)

using CueSheet::Segment;
using CueSheet::Sheet;

static int ms(const char* stamp, bool* ok = nullptr)
{
    return CueSheet::msFromTimecode(QString::fromLatin1(stamp), ok);
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // ---- 1. MM:SS:FF, where FF is FRAMES at 75/s -----------------------------------------------------
    // Every expected value is hand-computed: frames * 1000 / 75, rounded to the nearest millisecond.
    {
        bool ok = false;
        CHECK(ms("00:00:00", &ok) == 0);          CHECK(ok);
        CHECK(ms("00:01:00", &ok) == 1000);       CHECK(ok);
        CHECK(ms("01:00:00", &ok) == 60000);      CHECK(ok);
        CHECK(ms("02:03:00", &ok) == 123000);     CHECK(ok);

        // THE FRAMES. 1/75 s == 13.33 ms; 2/75 == 26.67; 37/75 == 493.33; 74/75 == 986.67. A reader that
        // treated FF as HUNDREDTHS would answer 10 / 20 / 370 / 740 here, and a reader that treated it as
        // MILLISECONDS would answer 1 / 2 / 37 / 74 — both playable, both wrong, neither noisy.
        CHECK(ms("00:00:01") == 13);
        CHECK(ms("00:00:02") == 27);              // rounded UP from 26.67: truncation would say 26
        CHECK(ms("00:00:37") == 493);
        CHECK(ms("00:00:74") == 987);             // the largest legal frame, rounded up from 986.67
        CHECK(ms("03:12:37") == 192493);          // the header's worked example

        // FRAME 75 DOES NOT EXIST. A CD second holds 0..74, and a sheet that says otherwise has arithmetic
        // we should not adopt — refusing it is what makes the track unplaceable and the sheet invalid.
        ok = true;  CHECK(ms("00:00:75", &ok) == 0);  CHECK(!ok);
        ok = true;  CHECK(ms("00:60:00", &ok) == 0);  CHECK(!ok);   // 60 seconds is a minute

        // MM:SS (no frames) is accepted: several taggers write it.
        ok = false; CHECK(ms("04:30", &ok) == 270000); CHECK(ok);
        // Long single-file rips run past 99 minutes, so the minute field is not two digits wide.
        ok = false; CHECK(ms("123:45:00", &ok) == 7425000); CHECK(ok);
        // …but not unbounded: the milliseconds are a signed int, and a sheet claiming a thousand hours must
        // be refused rather than multiplied into a negative timestamp.
        ok = true;  CHECK(ms("60000:00:00", &ok) == 0); CHECK(!ok);

        // Everything else is refused rather than guessed at.
        for (const char* bad : { "", "abc", "1:2:3:4", "-1:00:00", "00: 0:00", "00:0a:00", "1.5:00:00" })
        {
            ok = true;
            const int v = CueSheet::msFromTimecode(QString::fromLatin1(bad), &ok);
            CHECK(!ok);
            CHECK(v == 0);
        }
    }

    // ---- 2. An ordinary sheet, in the shape real ones come in ----------------------------------------
    // CRLF, a REM block, indentation, quoted values, a track-level PERFORMER overriding the disc's, an
    // INDEX 00 pregap on track 2, and an ISRC to be ignored.
    const QString ordinary = QStringLiteral(
        "REM GENRE \"Alternative Rock\"\r\n"
        "REM DATE 1997\r\n"
        "REM COMMENT \"ExactAudioCopy v0.99\"\r\n"
        "PERFORMER \"Portishead\"\r\n"
        "TITLE \"Dummy\"\r\n"
        "FILE \"Dummy.wav\" WAVE\r\n"
        "  TRACK 01 AUDIO\r\n"
        "    TITLE \"Mysterons\"\r\n"
        "    ISRC GBAAA9700001\r\n"
        "    INDEX 01 00:00:00\r\n"
        "  TRACK 02 AUDIO\r\n"
        "    TITLE \"Sour Times\"\r\n"
        "    PERFORMER \"Beth Gibbons\"\r\n"
        "    INDEX 00 05:00:00\r\n"
        "    INDEX 01 05:02:37\r\n"
        "  TRACK 03 AUDIO\r\n"
        "    TITLE \"Strangers\"\r\n"
        "    INDEX 01 09:11:00\r\n");
    {
        const Sheet s = CueSheet::parse(ordinary);
        CHECK(s.isValid());
        CHECK(s.title == QStringLiteral("Dummy"));
        CHECK(s.performer == QStringLiteral("Portishead"));
        CHECK(s.genre == QStringLiteral("Alternative Rock"));
        CHECK(s.year == 1997);
        CHECK(s.files.size() == 1);
        CHECK(s.files.first().name == QStringLiteral("Dummy.wav"));
        CHECK(s.trackCount() == 3);

        const auto& tr = s.files.first().tracks;
        CHECK(tr.at(0).number == 1);
        CHECK(tr.at(0).title == QStringLiteral("Mysterons"));
        CHECK(tr.at(0).isrc == QStringLiteral("GBAAA9700001"));
        CHECK(tr.at(0).startMs == 0);
        CHECK(tr.at(0).pregapMs == -1);
        CHECK(tr.at(1).performer == QStringLiteral("Beth Gibbons"));
        CHECK(tr.at(1).startMs == 302493);            // 5:02 + 37/75 s
        CHECK(tr.at(1).pregapMs == 300000);           // the INDEX 00, read and reported
        CHECK(tr.at(2).startMs == 551000);            // 9:11

        // ---- 4. THE BOUNDARY IS INDEX 01, NOT THE PREGAP -----------------------------------------------
        // Track 1 must run to 302493 — where the music of track 2 starts — and NOT to 300000, which is
        // where its run-in begins. mpv's own cue demuxer does it this way; the header says why agreeing
        // with it is worth more than a second opinion.
        const QVector<Segment> segs = CueSheet::singleFileSegments(s);
        CHECK(segs.size() == 3);
        CHECK(segs.at(0).startMs == 0);
        CHECK(segs.at(0).endMs == 302493);
        CHECK(segs.at(1).startMs == 302493);
        CHECK(segs.at(1).endMs == 551000);
        CHECK(segs.at(2).startMs == 551000);
        CHECK(segs.at(2).endMs == -1);                // the last track runs to the end of the file

        // The performer fallback is applied HERE so no caller repeats it.
        CHECK(segs.at(0).performer == QStringLiteral("Portishead"));
        CHECK(segs.at(1).performer == QStringLiteral("Beth Gibbons"));
        CHECK(segs.at(2).performer == QStringLiteral("Portishead"));
        CHECK(segs.at(1).title == QStringLiteral("Sour Times"));
        CHECK(segs.at(2).number == 3);
    }

    // ---- 2b. The awkward shapes ----------------------------------------------------------------------
    {
        // UNQUOTED values run to the end of the line; a FILE's trailing format word is dropped, but only
        // when it really is one.
        const Sheet s = CueSheet::parse(QStringLiteral(
            "TITLE The Dark Side of the Moon\n"
            "PERFORMER Pink Floyd\n"
            "SONGWRITER Roger Waters\n"
            "FILE Dark Side.flac WAVE\n"
            "TRACK 01 AUDIO\n"
            "TITLE Speak to Me\n"
            "INDEX 01 00:00:00\n"
            "TRACK 02 AUDIO\n"
            "TITLE Breathe\n"
            "INDEX 01 01:07:33\n"));
        CHECK(s.isValid());
        CHECK(s.title == QStringLiteral("The Dark Side of the Moon"));
        CHECK(s.performer == QStringLiteral("Pink Floyd"));
        CHECK(s.songwriter == QStringLiteral("Roger Waters"));
        CHECK(s.files.first().name == QStringLiteral("Dark Side.flac"));   // "WAVE" dropped, the space kept
        CHECK(s.files.first().tracks.at(1).startMs == 67440);              // 1:07 + 33/75 == 67.44 s
        CHECK(CueSheet::singleFileSegments(s).at(0).songwriter == QStringLiteral("Roger Waters"));
    }
    {
        // A file genuinely named after a format word keeps its name; a lone-CR sheet still has lines; a
        // title whose closing quote went missing is odd, not fatal.
        const Sheet s = CueSheet::parse(QStringLiteral(
            "FILE \"Live WAVE.flac\" WAVE\r"
            "TRACK 01 AUDIO\r"
            "TITLE \"Opening\r"
            "INDEX 01 00:00:00\r"
            "TRACK 02 AUDIO\r"
            "INDEX 01 00:30:00\r"));
        CHECK(s.isValid());
        CHECK(s.files.first().name == QStringLiteral("Live WAVE.flac"));
        // An UNQUOTED name with no format word after it keeps its last word. Dropping it unconditionally
        // would rename "Live At Leeds.flac" to "Live At" and the sheet would then be about nothing.
        CHECK(CueSheet::parse(QStringLiteral("FILE Live At Leeds.flac\nTRACK 01 AUDIO\nINDEX 01 00:00:00\n"))
                  .files.first().name == QStringLiteral("Live At Leeds.flac"));
        CHECK(s.files.first().tracks.at(0).title == QStringLiteral("Opening"));
        CHECK(s.files.first().tracks.at(1).title.isEmpty());   // untitled is legal; the library numbers it
        CHECK(CueSheet::singleFileSegments(s).size() == 2);
    }
    {
        // A track with ONLY a pregap is still placed by it — a sheet that writes no INDEX 01 is unusual,
        // not unreadable.
        const Sheet s = CueSheet::parse(QStringLiteral(
            "FILE \"a.flac\" WAVE\nTRACK 01 AUDIO\nINDEX 00 00:00:00\n"
            "TRACK 02 AUDIO\nINDEX 00 02:00:00\n"));
        CHECK(s.isValid());
        CHECK(s.files.first().tracks.at(1).startMs == 120000);
    }
    {
        // A MIXED-MODE disc's data track is a filesystem, not music. It is skipped, and its INDEX line does
        // not land on the audio track before it.
        const Sheet s = CueSheet::parse(QStringLiteral(
            "FILE \"mixed.bin\" BINARY\n"
            "TRACK 01 MODE1/2352\n  INDEX 01 00:00:00\n"
            "TRACK 02 AUDIO\n  INDEX 01 01:00:00\n"
            "TRACK 03 AUDIO\n  INDEX 01 02:00:00\n"));
        CHECK(s.trackCount() == 2);
        CHECK(s.files.first().tracks.at(0).number == 2);
        CHECK(s.files.first().tracks.at(0).startMs == 60000);
    }

    // ---- 3. Encodings ---------------------------------------------------------------------------------
    {
        const QString body = QStringLiteral(
            "PERFORMER \"Bj\u00F6rk\"\nFILE \"a.flac\" WAVE\n"
            "TRACK 01 AUDIO\nTITLE \"Hyperballad\"\nINDEX 01 00:00:00\n"
            "TRACK 02 AUDIO\nINDEX 01 01:00:00\n");

        // UTF-8, no BOM: strict decode succeeds and the name is spelled right.
        CHECK(CueSheet::parseBytes(body.toUtf8()).performer == QStringLiteral("Bj\u00F6rk"));
        // UTF-8 with a BOM.
        CHECK(CueSheet::parseBytes(QByteArray("\xEF\xBB\xBF") + body.toUtf8()).performer
              == QStringLiteral("Bj\u00F6rk"));
        // WINDOWS-1252 / Latin-1, no BOM. 0xF6 alone is not valid UTF-8, so the strict attempt fails and the
        // Latin-1 fallback reads it correctly — a sheet that would otherwise be mojibake or refused.
        const QByteArray latin1 = body.toLatin1();
        CHECK(latin1.contains('\xF6'));
        CHECK(CueSheet::parseBytes(latin1).performer == QStringLiteral("Bj\u00F6rk"));
        // UTF-16 with a BOM, both ways round: the BOM answers outright.
        QByteArray le("\xFF\xFE", 2);
        for (const QChar c : body) { le.append(char(c.unicode() & 0xFF)); le.append(char(c.unicode() >> 8)); }
        CHECK(CueSheet::parseBytes(le).performer == QStringLiteral("Bj\u00F6rk"));
        QByteArray be("\xFE\xFF", 2);
        for (const QChar c : body) { be.append(char(c.unicode() >> 8)); be.append(char(c.unicode() & 0xFF)); }
        CHECK(CueSheet::parseBytes(be).performer == QStringLiteral("Bj\u00F6rk"));
        // All five agree about the MUSIC as well as the spelling.
        CHECK(CueSheet::singleFileSegments(CueSheet::parseBytes(latin1)).size() == 2);
    }

    // ---- 5. Refusal: an invalid sheet yields NOTHING, never a guess -----------------------------------
    {
        // Nothing at all, and something that is not a cue sheet.
        CHECK(!CueSheet::parse(QString()).isValid());
        CHECK(!CueSheet::parse(QStringLiteral("<html><body>404 Not Found</body></html>")).isValid());
        CHECK(CueSheet::singleFileSegments(CueSheet::parse(QStringLiteral("garbage"))).isEmpty());

        // A track with no INDEX at all cannot be placed. The whole sheet is refused, because the
        // alternative is an album whose fourth track silently starts in the wrong place.
        const Sheet noIndex = CueSheet::parse(QStringLiteral(
            "FILE \"a.flac\" WAVE\nTRACK 01 AUDIO\nINDEX 01 00:00:00\n"
            "TRACK 02 AUDIO\nTITLE \"No index here\"\n"
            "TRACK 03 AUDIO\nINDEX 01 02:00:00\n"));
        CHECK(noIndex.trackCount() == 3);
        CHECK(!noIndex.isValid());
        CHECK(CueSheet::singleFileSegments(noIndex).isEmpty());

        // The SAME failure with the unplaced track FIRST, where a monotonicity check with the wrong starting
        // value would wave it through and produce a segment beginning at millisecond -1. Both cases are one
        // rule in isValid(), and this is the half that proves the rule's starting value is load-bearing.
        const Sheet firstUnplaced = CueSheet::parse(QStringLiteral(
            "FILE \"a.flac\" WAVE\nTRACK 01 AUDIO\nTITLE \"No index here\"\n"
            "TRACK 02 AUDIO\nINDEX 01 01:00:00\n"
            "TRACK 03 AUDIO\nINDEX 01 02:00:00\n"));
        CHECK(firstUnplaced.trackCount() == 3);
        CHECK(firstUnplaced.files.first().tracks.at(0).startMs == -1);
        CHECK(!firstUnplaced.isValid());
        CHECK(CueSheet::singleFileSegments(firstUnplaced).isEmpty());

        // An out-of-range frame count is the same failure: the INDEX is unreadable, so the track is unplaced.
        const Sheet badFrames = CueSheet::parse(QStringLiteral(
            "FILE \"a.flac\" WAVE\nTRACK 01 AUDIO\nINDEX 01 00:00:00\n"
            "TRACK 02 AUDIO\nINDEX 01 01:00:99\n"));
        CHECK(!badFrames.isValid());
        CHECK(CueSheet::singleFileSegments(badFrames).isEmpty());

        // Backwards timestamps, and two tracks starting at the same instant (a track of zero length).
        CHECK(!CueSheet::parse(QStringLiteral(
            "FILE \"a.flac\" WAVE\nTRACK 01 AUDIO\nINDEX 01 02:00:00\n"
            "TRACK 02 AUDIO\nINDEX 01 01:00:00\n")).isValid());
        CHECK(!CueSheet::parse(QStringLiteral(
            "FILE \"a.flac\" WAVE\nTRACK 01 AUDIO\nINDEX 01 01:00:00\n"
            "TRACK 02 AUDIO\nINDEX 01 01:00:00\n")).isValid());

        // Tracks before any FILE belong to nothing and are dropped rather than attached to one that does
        // not exist. What is left here is a one-track file, so nothing is expanded.
        const Sheet orphan = CueSheet::parse(QStringLiteral(
            "TRACK 01 AUDIO\nINDEX 01 00:00:00\n"
            "FILE \"a.flac\" WAVE\nTRACK 02 AUDIO\nINDEX 01 01:00:00\n"));
        CHECK(orphan.trackCount() == 1);
        CHECK(CueSheet::singleFileSegments(orphan).isEmpty());
    }

    // ---- 6. "This cue changes nothing" ----------------------------------------------------------------
    {
        // A cue over PER-TRACK FILES. That album is already a folder of files and the scanner already gets
        // it right; expanding it would invent tracks inside files that hold one song each.
        const Sheet multi = CueSheet::parse(QStringLiteral(
            "FILE \"01.flac\" WAVE\nTRACK 01 AUDIO\nINDEX 01 00:00:00\n"
            "FILE \"02.flac\" WAVE\nTRACK 02 AUDIO\nINDEX 01 00:00:00\n"));
        CHECK(multi.isValid());                       // it is a perfectly good sheet…
        CHECK(multi.files.size() == 2);
        CHECK(CueSheet::singleFileSegments(multi).isEmpty());   // …and it changes nothing here

        // A TWO-DISC SET as two big files under ONE sheet. Each FILE section really does describe a
        // single-file rip, and this is still left alone on purpose — the header says what that costs. It is
        // asserted rather than merely true, because "expand the first FILE and ignore the rest" is the shape
        // a plausible loosening of the rule would take, and it would silently drop half the album.
        const Sheet twoDiscs = CueSheet::parse(QStringLiteral(
            "FILE \"CD1.flac\" WAVE\n"
            "TRACK 01 AUDIO\nINDEX 01 00:00:00\nTRACK 02 AUDIO\nINDEX 01 04:00:00\n"
            "FILE \"CD2.flac\" WAVE\n"
            "TRACK 03 AUDIO\nINDEX 01 00:00:00\nTRACK 04 AUDIO\nINDEX 01 03:30:00\n"));
        CHECK(twoDiscs.isValid());
        CHECK(twoDiscs.trackCount() == 4);
        CHECK(twoDiscs.files.first().tracks.size() == 2);
        CHECK(CueSheet::singleFileSegments(twoDiscs).isEmpty());

        // A ONE-TRACK sheet describes the file we already have.
        const Sheet single = CueSheet::parse(QStringLiteral(
            "FILE \"a.flac\" WAVE\nTRACK 01 AUDIO\nTITLE \"Only\"\nINDEX 01 00:00:00\n"));
        CHECK(single.isValid());
        CHECK(CueSheet::singleFileSegments(single).isEmpty());
    }

    // ---- 6b. Which file a sheet is about --------------------------------------------------------------
    {
        const Sheet s = CueSheet::parse(QStringLiteral(
            "FILE \"D:\\rips\\Dummy.wav\" WAVE\nTRACK 01 AUDIO\nINDEX 01 00:00:00\n"
            "TRACK 02 AUDIO\nINDEX 01 01:00:00\n"));
        CHECK(CueSheet::namesAudioFile(s, QStringLiteral("Dummy.wav")));
        // The ripping machine's absolute path is not ours; only the last component is compared.
        CHECK(CueSheet::namesAudioFile(s, QStringLiteral("dummy.WAV")));
        // Transcoded: the sheet still names the .wav it came from. The BASE name identifies the rip, and
        // refusing this would fail on the majority of real single-file rips.
        CHECK(CueSheet::namesAudioFile(s, QStringLiteral("Dummy.flac")));
        // A sheet naming a file that is not the one in front of us claims nothing — which is how a cue
        // pointing at something no longer on the disk quietly describes no album at all.
        CHECK(!CueSheet::namesAudioFile(s, QStringLiteral("Something Else.flac")));
        CHECK(!CueSheet::namesAudioFile(s, QString()));
    }

    // ---- 7. The clip url (mpv EDL) --------------------------------------------------------------------
    {
        // The plain case: `edl://%<bytes>%<path>,<start>,<length>;` with both times in seconds.
        CHECK(CueSheet::mpvClipUrl(QStringLiteral("C:/m/a.flac"), 302493, 551000)
              == QStringLiteral("edl://%11%C:/m/a.flac,302.493,248.507;"));
        // Track 1 starts at zero and still carries an explicit start.
        CHECK(CueSheet::mpvClipUrl(QStringLiteral("C:/m/a.flac"), 0, 302493)
              == QStringLiteral("edl://%11%C:/m/a.flac,0.000,302.493;"));

        // THE LAST TRACK CARRIES NO LENGTH. mpv then plays to the end of the file; a faked length is
        // reported as the DURATION (`…,240,999;` loads as 999 s on a 300 s file), so the progress bar would
        // lie for the whole closing song. And it must not leave a dangling comma, which mpv refuses to load.
        const QString last = CueSheet::mpvClipUrl(QStringLiteral("C:/m/a.flac"), 551000, -1);
        CHECK(last == QStringLiteral("edl://%11%C:/m/a.flac,551.000;"));
        CHECK(!last.contains(QStringLiteral(",;")));

        // THE LENGTH PREFIX IS WHY A COMMA IN A PATH IS SAFE. Unquoted, mpv would split this into three
        // fields and load nothing.
        const QString comma = QStringLiteral("C:/m/Now, That's What I Call Music.flac");
        CHECK(comma.toUtf8().size() == 39);
        CHECK(CueSheet::mpvClipUrl(comma, 1000, 2000)
              == QStringLiteral("edl://%39%C:/m/Now, That's What I Call Music.flac,1.000,1.000;"));

        // THE COUNT IS IN BYTES, NOT CHARACTERS. mpv counts the octets it is handed, and the two differ the
        // moment a path has an accent in it — an off-by-two prefix truncates the filename.
        const QString accent = QStringLiteral("C:/m/Bj\u00F6rk.flac");
        CHECK(accent.size() == 15);
        CHECK(accent.toUtf8().size() == 16);
        CHECK(CueSheet::mpvClipUrl(accent, 0, 1000).startsWith(QStringLiteral("edl://%16%")));

        // Degenerate inputs answer something loadable or nothing at all, never a malformed url.
        CHECK(CueSheet::mpvClipUrl(QString(), 0, 1000).isEmpty());
        CHECK(CueSheet::mpvClipUrl(QStringLiteral("a.flac"), 5000, 5000)
              == QStringLiteral("edl://%6%a.flac,5.000;"));      // zero-length span == play to the end
        CHECK(CueSheet::mpvClipUrl(QStringLiteral("a.flac"), -1, 1000)
              == QStringLiteral("edl://%6%a.flac,0.000,1.000;")); // a negative start is clamped, not emitted
    }

    // ---- 8. load(): the one function that touches a disk ----------------------------------------------
    {
        const QString dir = QDir::tempPath() + QStringLiteral("/eb-probe-cue");
        QDir().mkpath(dir);
        const QString path = dir + QStringLiteral("/Album.cue");
        QFile f(path);
        CHECK(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(ordinary.toUtf8());
        f.close();

        const Sheet s = CueSheet::load(path);
        CHECK(s.isValid());
        CHECK(s.title == QStringLiteral("Dummy"));
        CHECK(CueSheet::singleFileSegments(s).size() == 3);

        // A path that is not there is an empty, invalid sheet — never a throw, never a crash. A scan walks
        // whatever a user's disk happens to contain.
        CHECK(!CueSheet::load(dir + QStringLiteral("/missing.cue")).isValid());
        CHECK(!CueSheet::load(dir).isValid());          // a directory
        CHECK(!CueSheet::load(QString()).isValid());

        QFile::remove(path);
        QDir().rmdir(dir);
    }

    if (g_fails == 0)
        std::printf("CUE-OK\n");
    else
        std::printf("CUE had %d failure(s)\n", g_fails);
    return g_fails == 0 ? 0 : 1;
}
