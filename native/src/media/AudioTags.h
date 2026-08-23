// Audio TAG READING for local music files (issue #74, increment 1) — one file in, one value type out.
//
// Nothing else in this tree reads an audio tag. A user who points the app at a music folder gets a file
// browser rather than a library precisely because this layer did not exist, so everything the music library
// will grow on top — the incremental scan, the Artists -> Albums -> Tracks browse, and ReplayGain (#141) —
// reads from HERE and nowhere else. That is the reason this is a free function over a value type rather than
// a class with state: a scan calls it once per file from a worker thread and keeps the struct, and nothing it
// returns points back at a file, a TagLib object or a thread.
//
// WHY TagLib AND NOT MPV. libmpv can report a loaded file's metadata, but only for the file it has LOADED —
// one demuxer, one playback pipeline, one file at a time. A library scan reads thousands of files and plays
// none of them. TagLib parses the container's tag block directly and is the boring, correct choice; the build
// pulls it at a pinned tag (native/CMakeLists.txt, the taglib FetchContent block).
//
// WHAT IS READ IN ONE PASS. title / artist / ALBUM ARTIST / album / track / disc / year / genre, the embedded
// cover, the duration, the four ReplayGain values, AND the embedded lyrics. ReplayGain belongs to a different
// issue (#141) and lyrics to another again (#142), but both come out of the same tag block in the same read:
// a second pass over the whole library later, to pick up values that were sitting in the map we already
// built, would be pure waste. #142 says so in as many words — embedded lyrics arrive "via the same TagLib
// pass #74 already require" — so there is no second reader and no other place in the tree that opens an audio
// file to look for words.
//
// ALBUM ARTIST IS A FIRST-CLASS FIELD, not a fallback computed at the call site. On a compilation every track
// has a different `artist` and the same `albumArtist` ("Various Artists"), and a browse that groups on
// `artist` shatters that album into one album per track — the classic first bug of every music library, and
// the one #74 calls out by name. The raw value is kept exactly as tagged (empty when absent) and
// effectiveAlbumArtist() applies the fallback, so a caller cannot get the grouping key by accident.
//
// ARTIST AND GENRE ARE MULTI-VALUED; ALBUM ARTIST IS NOT (issue #196, part 1). A track credited to three
// people is three artists, and until this existed it was one string — "Artist A; Artist B" became its own
// artist entry, distinct from either, so browsing Artist A did not find the track. `artists` and `genres`
// below are the real lists; `artist` and `genre` stay as the single display strings everything already reads.
// ALBUM ARTIST IS DELIBERATELY EXEMPT and stays one string: it is the album GROUPING key (MusicLibrary.h says
// why), and splitting it would file one album per credited performer — the same shattered-album bug #74
// fought, arriving from the opposite direction.
//
// THE CLASSICAL FIELDS (issue #196, part 2): COMPOSER, CONDUCTOR, PERFORMER, WORK and MOVEMENT. Classical
// listeners are the worst-served audience in any artist/album-shaped library because ARTIST is the wrong
// axis — the interesting questions are "what did this composer write", "who conducted it" and "who is
// playing". None of that needs a new read: every one of these values is sitting in the property map this
// pass already built, so they come out of the SAME read as the title, exactly as ReplayGain and the lyrics
// do. Composer/conductor/performer are MULTI-VALUED and go through the same two-step rule as artist below —
// a concerto has two soloists as routinely as a pop track has two rappers. WORK and MOVEMENT are single
// strings: a track belongs to one work and is one movement of it, and splitting either would invent a
// second piece of music that nobody recorded.
//
// WHAT THE CONTAINERS CALL THEM. TagLib normalises to one vocabulary — ID3v2's TCOM/TPE3/TIT1 arrive as
// COMPOSER/CONDUCTOR/WORK, MP4's ©wrt/©cnd/©wrk likewise, and a Vorbis comment already uses those names.
// MOVEMENT is read from MOVEMENTNAME (ID3v2's MVNM, MP4's ©mvn, Picard's Vorbis spelling) and falls back to
// a plain MOVEMENT, which is what several older taggers wrote. PERFORMER is the one field with no single
// spelling: Vorbis and MP4 have a plain PERFORMER, while ID3v2 has no such frame at all and stores players
// in TMCL — one entry PER INSTRUMENT, which TagLib folds into "PERFORMER:<instrument>" keys. Those keyed
// entries are gathered too and the instrument is dropped, because the dimension is the person; otherwise
// every classical mp3 in the world would report no performers at all.
//
// WHERE THE VALUES COME FROM, in order:
//   1. THE CONTAINER'S OWN STRUCTURE, always preferred. A Vorbis comment block REPEATS the field
//      ("ARTIST=A" then "ARTIST=B"); an ID3v2.4 text frame separates values with a NUL byte. TagLib parses
//      both into a StringList per key, so those files are never string-split — the format already answered
//      the question, and re-splitting its answer could only lose.
//   2. AN AD-HOC SEPARATOR, and only when step 1 produced exactly ONE value. ID3v2.3 has no multi-value
//      encoding at all, so every tagger that has to flatten a list into one string picks a character, and
//      the file cannot say which. Hence `separators` is a parameter, defaulting to NOTHING here: this reader
//      holds no policy. Settings::musicTagSeparators() owns the default the app scans with (and the comment
//      there defends it); a caller that passes nothing gets structured values only, which is always safe.
//
// SPLITTING IS THE PART WITH JUDGEMENT IN IT, so splitTagValues() is public and probed directly. Two rules
// keep it from shredding names: a PUNCTUATION separator (";", "/", "|") matches literally, while a separator
// containing a LETTER ("feat.", "ft.", "vs.", "and") only matches with whitespace on both sides — otherwise
// "feat." would cut "Featherstone" in half and "and" would cut "Bandwagon". Nothing protects a band whose
// name contains a punctuation separator, which is exactly why the app's default list is one character long.
//
// FAILURE IS AN EMPTY RESULT, NEVER AN EXCEPTION. A missing file, a directory, a zero-byte file, a truncated
// download, an mp3 that is actually an HTML error page — every one of them returns a default-constructed
// Tags with isEmpty() true. A scan walks whatever a user's disk happens to contain, so "this file is
// nonsense" has to be an ordinary, cheap answer rather than something a caller can forget to catch.
#pragma once
#include <QByteArray>
#include <QString>
#include <QStringList>

namespace AudioTags
{
    // An embedded cover image exactly as the container stored it — the bytes are the encoded JPEG/PNG, not a
    // decoded image, because the scan runs off the GUI thread and QImage decoding is the caller's decision.
    struct Picture
    {
        QByteArray data;     // encoded image bytes
        QString    mimeType; // "image/jpeg", "image/png"; sniffed from the bytes when the tag omits it

        bool isNull() const { return data.isEmpty(); }
    };

    // A ReplayGain number with its PRESENCE, because 0 is a real value for both halves: a track that needs no
    // adjustment is tagged "0.00 dB", and "no tag at all" must not read as "apply 0 dB" — the player has to be
    // able to tell "already normalised" from "unknown" to decide whether to fall back to a preamp default.
    struct GainValue
    {
        bool   present = false;
        double value   = 0.0; // gain in dB; peak as the linear sample ratio the spec stores (1.0 == full scale)
    };

    struct Tags
    {
        QString title;
        QString artist;      // the credit as ONE display string. When the container itself carried several
                             // values (a repeated Vorbis field, a NUL-separated ID3v2.4 frame) there is no
                             // single "raw" string to keep, so they are joined with "; " — the app's own
                             // default separator, so the display round-trips back through splitTagValues().
        QString albumArtist; // raw; empty when the file does not carry one. See effectiveAlbumArtist().
                             // SINGLE-VALUED ON PURPOSE — never split. See the header note.
        QString album;
        QString genre;       // as `artist`: one display string, joined when the container held several.

        // The multi-value views (issue #196). One entry per credited artist / per genre, in tag order,
        // trimmed, with case-insensitive duplicates collapsed to their first spelling. A single-valued file
        // yields exactly one entry, so a caller can read these unconditionally; an untagged field yields none.
        QStringList artists;
        QStringList genres;

        // THE CLASSICAL FIELDS (#196, part 2). Same shape as artist/genre above — a display string beside
        // the authoritative list — so a caller reads whichever it needs without asking which shape it got.
        // Every one of them is empty on a file that carries no such tag, which is most files in most
        // libraries: nothing downstream is allowed to change behaviour because these exist.
        QString     composer;    // display; joined with "; " when the file credited several
        QString     conductor;
        QString     performer;
        QStringList composers;
        QStringList conductors;
        QStringList performers;

        // SINGLE-VALUED, on purpose (see the header). `work` is the piece a track belongs to
        // ("Goldberg Variations, BWV 988"); `movement` is this track's part of it ("Variatio 1 a 1 Clav.").
        QString work;
        QString movement;

        int track      = 0; // 0 == untagged. "3/12" fills track=3 and trackTotal=12.
        int trackTotal = 0;
        int disc       = 0;
        int discTotal  = 0;
        int year       = 0; // the four-digit year out of a "1997" / "1997-05-13" / "1997-05-13T12:00" DATE

        int durationSec = 0; // 0 when the container cannot give it cheaply; never a reason to call this empty

        Picture cover;

        GainValue trackGain, albumGain; // REPLAYGAIN_*_GAIN, dB
        GainValue trackPeak, albumPeak; // REPLAYGAIN_*_PEAK, linear

        // EMBEDDED LYRICS (issue #142, source 2). Two fields rather than one because the containers really do
        // carry two different things, and only one of them is guaranteed to be timed:
        //
        //   syncedLyrics — ID3v2's SYLT frame, a list of (millisecond, text) pairs, RENDERED BACK TO LRC TEXT
        //                  ("[mm:ss.xx]line\n…") by LrcLyrics::renderLrc. It is LRC on the way out precisely
        //                  so nothing downstream grows a second lyric parser: SYLT is the only tag in any
        //                  container that is structurally timed, and this is where that structure is flattened
        //                  into the one format the app already reads. Empty when the file carries no SYLT.
        //
        //   lyrics       — the TEXT lyrics tag, exactly as stored: ID3v2 USLT, MP4's ©lyr atom, a Vorbis
        //                  LYRICS/UNSYNCEDLYRICS comment, WMA's WM/Lyrics. Deliberately NOT called
        //                  "unsyncedLyrics", because a large minority of files in the wild hold a full LRC
        //                  document in here — every tagger that writes lyrics from an .lrc does it this way.
        //                  Whether it is timed is not this reader's call to make; LrcLyrics::parseLrc decides,
        //                  by whether it finds a timestamp, and the same text yields a scrollable plain sheet
        //                  when it does not.
        //
        // NEITHER COUNTS TOWARDS isEmpty(), for the same reason duration does not: a lyric sheet is not
        // something a library can file a track under. A track with words and no title still belongs in the
        // filename fallback, not on a shelf of blank-titled entries.
        QString syncedLyrics;
        QString lyrics;

        // THE EMBEDDED CUE SHEET (issue #196, part 3). A single-file album rip carries its track list either
        // in an `Album.cue` beside the audio or in a CUESHEET tag INSIDE it — FLAC and APE rips written by
        // EAC and its descendants use the tag as often as the sidecar, and a library that only looked beside
        // the file would show half of them as one seventy-minute track. It is the sheet's TEXT, exactly as
        // stored, because parsing it is CueSheet's job and not a tag reader's; empty for every file that
        // carries no such tag, which is essentially every file in most libraries.
        //
        // It arrives here for the same reason ReplayGain and the lyrics do: it is sitting in the property
        // map this one pass already built, and a second walk of the library to collect it would be pure
        // waste. Like them it does NOT count towards isEmpty() — a rip with a cue sheet and no other tag
        // still wants the filename/folder fallbacks.
        QString cuesheet;

        // "This file told us nothing we could file it under." Duration is deliberately NOT part of it: an
        // untagged wav still has a length, and a library that treated a length as metadata would show a shelf
        // full of blank-titled entries instead of leaving them to the filename fallback the browse will do.
        //
        // THE CLASSICAL FIELDS ARE NOT PART OF IT EITHER (#196, part 2), and that is a decision rather than
        // an oversight. This verdict decides the ARTIST/ALBUM axis fallbacks — filename for a missing title,
        // containing folder for a missing album — and a file carrying a COMPOSER and nothing else needs both
        // of those just as much as a file carrying nothing at all. Counting the composer here would call such
        // a file "tagged" and then show it blank, which is the exact failure the exclusions above prevent.
        // It still reaches the Composers dimension, because that reads `composers`, not this.
        bool isEmpty() const
        {
            return title.isEmpty() && artist.isEmpty() && albumArtist.isEmpty() && album.isEmpty()
                && genre.isEmpty() && track == 0 && disc == 0 && year == 0 && cover.isNull()
                && !trackGain.present && !albumGain.present && !trackPeak.present && !albumPeak.present;
        }

        // The album-grouping key: the album artist when tagged, otherwise the track artist. Kept here rather
        // than at each call site so every surface groups a compilation the same way.
        //
        // The fallback takes the FIRST credited artist, not the whole `artist` string, and that one word is
        // the whole of #196's grouping fix: a file tagged ARTIST="A; B" with no ALBUMARTIST used to found an
        // artist called "A; B" that neither A nor B could reach. It now files under A, and B reaches the
        // track through the credit index (MusicLibrary::Artist::credits). When nothing split, artists.first()
        // IS `artist`, so no ordinary file moves.
        QString effectiveAlbumArtist() const
        {
            if (!albumArtist.isEmpty()) return albumArtist;
            return artists.isEmpty() ? artist : artists.first();
        }
    };

    // Reads one file. Never throws, never blocks on anything but the read, and returns an empty Tags for
    // anything it cannot make sense of.
    //
    // `separators` is the ad-hoc list for step 2 of the header's multi-value rule, and DEFAULTS TO EMPTY —
    // structured multi-values only. The scan passes Settings::musicTagSeparators(); a one-off read (cover
    // art, the now-playing panel) wants no policy and gets none.
    Tags read(const QString& filePath, const QStringList& separators = {});

    // One tag string -> its individual values, by the two rules in the header (punctuation matches anywhere,
    // a separator with a letter in it needs whitespace on both sides). Trimmed, empties dropped, case-
    // insensitive duplicates collapsed to their first spelling. An empty `separators` never splits, and a
    // string that is nothing but separators comes back as itself rather than as nothing.
    //
    // Public because it is the judgement call in this file and is probed on its own — and because a caller
    // holding a value from somewhere other than a tag block should split it by the same rule or not at all.
    QStringList splitTagValues(const QString& raw, const QStringList& separators);

    // Extension test for the scan to come — the cheap "is this even a music file" filter applied before a
    // file is opened at all. Extension-only on purpose: a scan of tens of thousands of files cannot afford to
    // open every .txt to find out, and read() is authoritative for anything that gets past this.
    bool isSupportedFile(const QString& filePath);
}
