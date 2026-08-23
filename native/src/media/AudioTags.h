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
// FAILURE IS AN EMPTY RESULT, NEVER AN EXCEPTION. A missing file, a directory, a zero-byte file, a truncated
// download, an mp3 that is actually an HTML error page — every one of them returns a default-constructed
// Tags with isEmpty() true. A scan walks whatever a user's disk happens to contain, so "this file is
// nonsense" has to be an ordinary, cheap answer rather than something a caller can forget to catch.
#pragma once
#include <QByteArray>
#include <QString>

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
        QString artist;
        QString albumArtist; // raw; empty when the file does not carry one. See effectiveAlbumArtist().
        QString album;
        QString genre;

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

        // "This file told us nothing we could file it under." Duration is deliberately NOT part of it: an
        // untagged wav still has a length, and a library that treated a length as metadata would show a shelf
        // full of blank-titled entries instead of leaving them to the filename fallback the browse will do.
        bool isEmpty() const
        {
            return title.isEmpty() && artist.isEmpty() && albumArtist.isEmpty() && album.isEmpty()
                && genre.isEmpty() && track == 0 && disc == 0 && year == 0 && cover.isNull()
                && !trackGain.present && !albumGain.present && !trackPeak.present && !albumPeak.present;
        }

        // The album-grouping key: the album artist when tagged, otherwise the track artist. Kept here rather
        // than at each call site so every surface groups a compilation the same way.
        QString effectiveAlbumArtist() const { return albumArtist.isEmpty() ? artist : albumArtist; }
    };

    // Reads one file. Never throws, never blocks on anything but the read, and returns an empty Tags for
    // anything it cannot make sense of.
    Tags read(const QString& filePath);

    // Extension test for the scan to come — the cheap "is this even a music file" filter applied before a
    // file is opened at all. Extension-only on purpose: a scan of tens of thousands of files cannot afford to
    // open every .txt to find out, and read() is authoritative for anything that gets past this.
    bool isSupportedFile(const QString& filePath);
}
