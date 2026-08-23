// CUE SHEET parsing (issue #196, part 3) — one .cue text in, an album's real track list out.
//
// A single-file rip is one enormous FLAC/APE/WAV plus an `Album.cue` beside it, and until this existed the
// music library showed it as ONE seventy-minute "track". The original media survey flagged it. Everything
// this feature needs from the sidecar — the track numbers, the titles, who is playing, and where each track
// STARTS inside the one file — is decided here and nowhere else, so the scan can build a real album without
// ever opening a player.
//
// WHY THIS FILE EXISTS AND MPV'S OWN CUE SUPPORT DOES NOT REPLACE IT. mpv does read a .cue natively, and the
// issue says so. What it produces, measured against libmpv on this machine (a five-track cue over a
// five-minute wav):
//
//     duration=300.000  chapters=5  playlist-count=1
//     chapter-list=[{"title":"One Two Twenty","time":0.0}, … {"title":"Six Sixty","time":240.0}]
//
// ONE playlist entry, three hundred seconds long, with the tracks as CHAPTERS. That is the seventy-minute
// item the issue is complaining about, wearing chapter marks. Handing it to PlaybackSession would queue one
// entry, fire one end-of-file at the end of the album, resume the album rather than the track, and make
// "play track 5" a chapter seek inside a single queue item — a second, chapter-shaped player mode beside the
// queue, which is exactly what the issue rules out. It is also unusable for the LIBRARY half at all: a scan
// reads thousands of files and plays none of them, which is the same argument AudioTags.h makes for reading
// tags with TagLib rather than with mpv, and it applies here verbatim.
//
// So the SHEET is parsed here, and mpv's native machinery is used for the half it is actually good at:
// clipping. See mpvClipUrl() at the bottom — the app never computes a boundary while something is playing.
//
// TIME IS MM:SS:FF AND FF IS FRAMES AT 75 PER SECOND. This is the one number in the feature that goes wrong
// quietly. A CD frame is 1/75 s, so `03:12:37` is 3 min 12 s and 37/75 s = 192493 ms — NOT 192370 ms (reading
// FF as hundredths) and NOT 192037 (as milliseconds). Every wrong reading still produces a playable album
// whose tracks all start slightly early or slightly late, nothing errors, and only a careful listener
// notices. msFromTimecode() is public and probed on its own for that reason, and the mutation matrix breaks
// the 75 specifically.
//
// INDEX 01 IS THE BOUNDARY; INDEX 00 IS PARSED AND MOVES NOTHING. A track may carry an INDEX 00 (the pregap:
// the run-in before the music starts) and an INDEX 01 (the audible start). Track N therefore ends where
// track N+1's INDEX 01 is, which leaves any pregap audio on the tail of the outgoing track. That is what
// mpv's own cue demuxer does — in the measurement above, track 2's INDEX 00 sat at 58 s and mpv still placed
// the chapter at 60 s — and agreeing with it is worth more than a defensible second opinion, because the two
// would otherwise disagree about the same album by two seconds. INDEX 00 is still READ, so that a sheet
// which writes only a pregap for a track has something to fall back on rather than being refused.
//
// A CUE OVER SEPARATE FILES IS NOT OUR PROBLEM. Plenty of sheets name one FILE per track; that album is
// already a folder of files and the scanner already gets it right. singleFileSegments() is the ONE call the
// library makes, and it answers "nothing to do" for those, for an invalid sheet, and for a sheet with fewer
// than two tracks — so the judgement about when a cue is even relevant lives here rather than being spread
// across the scan.
//
// THAT RULE IS DELIBERATELY BLUNT, and here is what it costs: a TWO-DISC set ripped as two big files under
// ONE sheet is also left alone, even though each of its FILE sections describes a real single-file rip. The
// common shape by far is one sheet per disc (`CD1.cue` beside `CD1.flac`), which works; the shared-sheet
// shape would need the scan to ask which FILE section a given audio file is under, and until somebody has
// one it is a rule with no library behind it. Leaving it alone shows what the library showed before, which
// is the safe half of the trade.
//
// REFUSED RATHER THAN GUESSED. Real .cue files are inconsistent, and this parser is deliberately generous
// about SHAPE and strict about TIME: unknown commands, missing quotes, CRLF, REM lines, stray indentation
// and blank lines are all ordinary, while a track whose start cannot be read is not. A sheet with a track it
// cannot place is invalid as a whole (isValid() false), because the alternative is an album with one track
// silently in the wrong place — and a wrong album is worse than the seventy-minute item this replaces.
//
// NOTHING HERE TOUCHES A FILE unless you call load(). parse()/parseBytes() are pure text in, value out, so
// the probe pins every rule above without audio, without a UI and without a disk.
#pragma once
#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVector>

namespace CueSheet
{
    // A CD frame. Spelled once, here, because it is the number the whole feature's correctness turns on.
    inline constexpr int kFramesPerSecond = 75;

    // One TRACK entry of one FILE. `startMs` is where the music starts; it is -1 only on a track this parser
    // could not place, which makes the whole sheet invalid.
    struct Track
    {
        int     number = 0;        // TRACK nn — as written, never renumbered
        QString title;             // TITLE, else empty
        QString performer;         // the TRACK's own PERFORMER, else empty (the sheet's is applied by segments())
        QString songwriter;        // SONGWRITER — the cue's word for the composer
        QString isrc;
        int     startMs  = -1;     // INDEX 01, else INDEX 00 when a sheet writes only that
        int     pregapMs = -1;     // INDEX 00 when present. Read, reported, and never a boundary — see header
    };

    // One FILE section and the tracks under it.
    struct File
    {
        QString name;              // the FILE "…" string exactly as the sheet wrote it
        QVector<Track> tracks;     // AUDIO tracks only; a mixed-mode disc's data track is not music
    };

    struct Sheet
    {
        QString title;             // the album
        QString performer;         // the album artist
        QString songwriter;
        QString genre;             // REM GENRE
        int     year = 0;          // REM DATE, when it holds a four-digit year
        QVector<File> files;

        int trackCount() const;

        // "Every track in this sheet has a place." At least one FILE, at least one track, every track placed,
        // and each file's starts strictly increasing. Two tracks at the same timestamp would be a track of
        // zero length, which is a broken sheet rather than a short song.
        bool isValid() const;
    };

    // The audible span of one track of a single-file rip, ready for the library.
    struct Segment
    {
        int     number = 0;
        QString title;
        QString performer;   // the track's own, else the sheet's — resolved here so no caller repeats the rule
        QString songwriter;  // likewise
        int     startMs = 0;
        int     endMs   = -1;  // -1 == "to the end of the file", which is the LAST track and only the last
    };

    // MM:SS:FF -> milliseconds, where FF is FRAMES AT 75/s. Also accepts MM:SS (some sheets drop the frames)
    // and an MMM:SS:FF with more than two minute digits, which long single-file rips need. `ok` is false for
    // anything else, INCLUDING a frame count of 75 or more: a CD second holds frames 0..74, and a sheet
    // saying otherwise is a sheet whose arithmetic we should not be adopting.
    //
    // Public because it is the judgement call in this file, is probed on its own, and is the value a wrong
    // answer would hide in — see the header.
    int msFromTimecode(const QString& text, bool* ok = nullptr);

    // Text in, sheet out. Pure.
    Sheet parse(const QString& text);

    // Bytes in, sheet out. The decode is part of the job rather than the caller's: .cue files are written by
    // ripping software going back thirty years and carry no encoding declaration, so the rule is a BOM when
    // there is one, then STRICT UTF-8, then Latin-1 — which cannot fail, so a Windows-1252 sheet full of
    // accented names still parses and merely spells one of them oddly, instead of being refused.
    Sheet parseBytes(const QByteArray& bytes);

    // Reads a .cue off disk (parseBytes over its contents). An unreadable path yields an empty, invalid Sheet.
    // The ONLY function here that touches a filesystem.
    Sheet load(const QString& cuePath);

    // Does this sheet name this audio file? Compared on each FILE entry's LAST path component,
    // case-insensitively, because sheets in the wild write absolute paths from the ripping machine and
    // backslashes on both platforms. A match on the base name alone counts too: a rip whose sheet still says
    // "Album.wav" beside the "Album.flac" it was transcoded into is the single commonest inconsistency there
    // is, and refusing it would fail on the majority of real single-file rips.
    bool namesAudioFile(const Sheet& sheet, const QString& audioFileName);

    // THE ONE CALL THE LIBRARY MAKES. The spans of a single-file rip's tracks, in sheet order, with the
    // performer/songwriter fallbacks already applied and the last track left open-ended.
    //
    // EMPTY — meaning "this cue changes nothing" — when the sheet is invalid, when it names more than one
    // FILE (that album is already a folder of files), or when it holds fewer than two tracks (a one-track
    // sheet describes the file we already have). Every "should we even do this" question is answered here.
    QVector<Segment> singleFileSegments(const Sheet& sheet);

    // A PLAYABLE HANDLE for one span of one file, as an mpv EDL url: `edl://%<bytes>%<path>,<start>[,<len>];`
    //
    // THIS IS THE "SEEK WITHIN THE SINGLE FILE, NEVER SPLIT IT" HALF, and it is mpv's own machinery rather
    // than ours: EDL is the timeline mechanism mpv's cue support is itself built on, so a clip reports the
    // TRACK's duration, seeks relative to the TRACK's start, and ends with an ordinary end-of-file at the
    // track boundary. Measured on this machine: `edl://Album.wav,120,60;` loads with duration=60.000 and a
    // seek to 30 lands at 30. That is what lets a cue track be an ordinary string in an ordinary
    // PlaybackSession queue — the app keeps no boundary bookkeeping of its own and has no second player mode.
    //
    // THE %<bytes>% PREFIX IS NOT OPTIONAL. An unquoted EDL entry is split on ',' and ';', so an album at
    // "…/Now, That's What I Call Music.flac" would be read as three fields and refuse to load. The count is
    // in BYTES of the UTF-8 the app hands mpv, not in QChars — the two differ the moment a path has an
    // accent in it, and mpv counts bytes.
    //
    // A LENGTH IS OMITTED, NEVER FAKED, for the last track (endMs < 0): mpv then plays to the end of the
    // file. Passing a too-large length instead makes mpv report THAT as the duration — `…,240,999;` loads
    // with duration=999 on a 300-second file — so the progress bar would lie for the whole of the last track.
    QString mpvClipUrl(const QString& filePath, int startMs, int endMs);
}
