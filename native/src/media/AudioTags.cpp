#include "AudioTags.h"

#include "LrcLyrics.h" // SYLT is rendered back to LRC text here, so the app keeps ONE lyric parser (#142)

#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <QVector>

#include <audioproperties.h>
#include <fileref.h>
#include <tfile.h>
#include <tpropertymap.h>
#include <tstringlist.h>
#include <tvariant.h>

// SYLT lives outside the property map (see readSylt below), so the ID3v2 side is reached directly. These are
// the only container-specific headers this file includes, and the CMake `tag` target had to grow the three
// build-tree include directories they live in — upstream's headers are flat only once installed.
#include <id3v2tag.h>
#include <mpegfile.h>
#include <synchronizedlyricsframe.h>

#include <string>

namespace
{
    // TagLib::String -> QString. to8Bit(true) is the UTF-8 encoding of whatever the tag actually held (Latin-1,
    // UTF-16 with a BOM, UTF-8), so this is the one conversion that survives a Japanese title and an accented
    // artist alike. Going via std::string rather than c_str() keeps an embedded NUL from truncating the value.
    QString qstr(const TagLib::String& s)
    {
        return QString::fromStdString(s.to8Bit(true));
    }

    // The whole property map, keyed by UPPERCASED tag name, first value only.
    //
    // Uppercasing is not tidiness. TagLib normalises the standard fields to uppercase keys for every container
    // (TPE2, aART and ALBUMARTIST all arrive as "ALBUMARTIST"), but the NON-standard ones keep the case the
    // file was written with: an ID3v2 TXXX frame becomes its description uppercased, while an MP4 freeform
    // atom becomes the atom's own suffix verbatim — and iTunes writes those lowercase. So the same ReplayGain
    // value is "REPLAYGAIN_TRACK_GAIN" in an mp3 and "replaygain_track_gain" in an m4a. Folding the keys once,
    // here, is what lets the rest of this file ask for one spelling and get both.
    //
    // First value only: TagLib returns a StringList per key because a tag may repeat a field (two ARTIST
    // comments in one Vorbis block). A library needs one name per column; multi-value handling is a browse
    // decision, not a parse decision, and nothing downstream asks for it yet.
    QHash<QString, QString> foldedProperties(const TagLib::PropertyMap& props)
    {
        QHash<QString, QString> out;
        for (const auto& entry : props)
        {
            if (entry.second.isEmpty())
                continue;
            const QString key = qstr(entry.first).toUpper();
            if (!out.contains(key))
                out.insert(key, qstr(entry.second.front()));
        }
        return out;
    }

    QString value(const QHash<QString, QString>& props, const char* key)
    {
        return props.value(QString::fromLatin1(key)).trimmed();
    }

    // The TEXT lyrics tag, whatever the container called it (issue #142, source 2).
    //
    // TagLib folds all four spellings onto ONE property key for us — ID3v2's USLT, MP4's ©lyr atom and WMA's
    // WM/Lyrics all arrive as "LYRICS", and a Vorbis LYRICS comment passes through verbatim — so the common
    // case is a single lookup. The two extra shapes are real files, not defensiveness:
    //   * "LYRICS:<DESCRIPTION>" — a USLT frame carrying a non-empty description keeps it in the key (TagLib
    //     does that so two USLT frames in different languages do not collide). Every value of that shape is
    //     still lyrics, so the first one is taken rather than none.
    //   * "UNSYNCEDLYRICS" — the Vorbis/APE spelling a handful of taggers write instead of LYRICS. It is not
    //     mapped to anything by TagLib because it is not standard, so it arrives under its own name.
    // NOT trimmed as a whole beyond the ends: the interior blank lines are the sheet's verse breaks.
    QString lyricsValue(const QHash<QString, QString>& props)
    {
        const QString direct = props.value(QStringLiteral("LYRICS"));
        if (!direct.trimmed().isEmpty())
            return direct.trimmed();
        const QString unsynced = props.value(QStringLiteral("UNSYNCEDLYRICS"));
        if (!unsynced.trimmed().isEmpty())
            return unsynced.trimmed();
        for (auto it = props.constBegin(); it != props.constEnd(); ++it)
            if (it.key().startsWith(QStringLiteral("LYRICS:")) && !it.value().trimmed().isEmpty())
                return it.value().trimmed();
        return {};
    }

    // ID3v2 SYLT -> LRC text (issue #142, source 2, the synced half).
    //
    // WHY THIS IS NOT A PROPERTY LOOKUP. SYLT is the one lyric tag with structure — a list of (timestamp,
    // text) pairs rather than a blob — and TagLib's property map is a string-to-string view, so SYLT is simply
    // absent from it. The frame has to be reached through the ID3v2 tag itself, which is why this is the only
    // container-specific code in the file. Only MPEG files are asked: SYLT is defined by ID3v2, and while
    // WAV/AIFF can technically carry an ID3v2 chunk, no tagger writes synced lyrics into one — an mp3 is what
    // the entire LRC-into-tags ecosystem produces.
    //
    // WHICH FRAME. Content type must be Lyrics (or Other, which is what a tagger that never set the byte
    // leaves behind); a transcription, a chord chart or a list of movements is not a lyric sheet and rendering
    // it as one would be a worse answer than showing nothing. The timestamp format must be milliseconds:
    // AbsoluteMpegFrames cannot be converted to seconds without the frame rate, and inventing one would put
    // every line at the wrong moment, which is the single most visible way a karaoke scroll can be wrong.
    //
    // HOW FRAGMENTS BECOME LINES. The ID3v2 spec's convention is that a fragment beginning with a newline
    // starts a new line, so a word-synced frame is (word)(word)(\nword)… — but the common whole-line writers
    // emit one fragment per line with NO leading newline, and merging those would collapse an entire song
    // into a single line. Neither shape can be assumed, so the shape is DETECTED: if any fragment after the
    // first announces itself with a newline, the newline convention is in force and unprefixed fragments
    // continue the line they follow (a word-level frame renders line-level, matching what parseLrc does with
    // enhanced <mm:ss.xx> word tags); otherwise every fragment is its own line.
    QString readSylt(TagLib::File* file)
    {
        auto* mpeg = dynamic_cast<TagLib::MPEG::File*>(file);
        if (!mpeg)
            return {};
        TagLib::ID3v2::Tag* id3 = mpeg->ID3v2Tag(false);
        if (!id3)
            return {};

        for (const auto* frame : id3->frameList("SYLT"))
        {
            const auto* sylt = dynamic_cast<const TagLib::ID3v2::SynchronizedLyricsFrame*>(frame);
            if (!sylt)
                continue;
            if (sylt->type() != TagLib::ID3v2::SynchronizedLyricsFrame::Lyrics
                && sylt->type() != TagLib::ID3v2::SynchronizedLyricsFrame::Other)
                continue;
            if (sylt->timestampFormat() != TagLib::ID3v2::SynchronizedLyricsFrame::AbsoluteMilliseconds)
                continue;

            const TagLib::ID3v2::SynchronizedLyricsFrame::SynchedTextList entries = sylt->synchedText();
            if (entries.isEmpty())
                continue;

            bool newlineConvention = false;
            bool first             = true;
            for (const auto& e : entries)
            {
                const QString t = qstr(e.text);
                if (!first && (t.startsWith(QChar('\n')) || t.startsWith(QStringLiteral("\r\n"))))
                    newlineConvention = true;
                first = false;
            }

            QVector<LrcLyrics::LyricLine> lines;
            for (const auto& e : entries)
            {
                QString text = qstr(e.text);
                const bool startsLine = !newlineConvention || lines.isEmpty()
                                     || text.startsWith(QChar('\n')) || text.startsWith(QStringLiteral("\r\n"));
                // Interior newlines become spaces either way: an LRC line is one line by construction, and a
                // raw '\n' inside a rendered line would parse back as a second, untimed line.
                text.replace(QStringLiteral("\r\n"), QStringLiteral(" "));
                text.replace(QChar('\n'), QChar(' '));
                text.replace(QChar('\r'), QChar(' '));
                text = text.trimmed();

                if (startsLine)
                    lines.push_back({ double(e.time) / 1000.0, text });
                else if (!text.isEmpty())
                    lines.back().text = (lines.back().text.isEmpty() ? text : lines.back().text + QChar(' ') + text);
            }
            // Drop lines that carried no words. A SYLT frame routinely ends with an empty terminating fragment,
            // and a blank timed line would show as a gap the highlight lands on with nothing in it.
            QVector<LrcLyrics::LyricLine> kept;
            for (const LrcLyrics::LyricLine& ln : lines)
                if (!ln.text.isEmpty())
                    kept.push_back(ln);
            if (!kept.isEmpty())
                return LrcLyrics::renderLrc(kept);
        }
        return {};
    }

    // "3/12" -> 3 and 12; "3" -> 3 and 0. Both halves of the pair live in one tag field in every container we
    // read (ID3v2 TRCK, Vorbis TRACKNUMBER, and MP4's trkn atom, which TagLib renders back into "3/12"), so
    // splitting it is the reader's job and not something a caller should have to know.
    void parsePair(const QString& text, int& number, int& total)
    {
        if (text.isEmpty())
            return;
        const int slash = text.indexOf(QChar('/'));
        bool ok = false;
        const int n = (slash < 0 ? text : text.left(slash)).trimmed().toInt(&ok);
        if (ok && n > 0)
            number = n;
        if (slash >= 0)
        {
            const int t = text.mid(slash + 1).trimmed().toInt(&ok);
            if (ok && t > 0)
                total = t;
        }
    }

    // The four-digit year out of a DATE field. The spec-shaped value is a full ISO timestamp
    // ("1997-05-13 12:00:00"), taggers write a bare year, and ID3v2.3's TYER is a bare year by definition —
    // all three start with the year, so take the leading digits and refuse anything that is not a plausible
    // year rather than storing a 5 from "5/13/97".
    int parseYear(const QString& text)
    {
        int i = 0;
        while (i < text.size() && text.at(i).isDigit())
            ++i;
        if (i != 4)
            return 0;
        const int y = text.left(4).toInt();
        return (y >= 1000 && y <= 9999) ? y : 0;
    }

    // "-7.15 dB" -> -7.15. The ReplayGain spec stores gains as a decimal with a " dB" suffix and peaks as a
    // bare decimal, and real files disagree about the space and the case ("+3.2dB", "-7.15 DB"), so strip a
    // trailing dB in any spelling and parse what is left. A value that will not parse stays ABSENT rather than
    // becoming 0.0, which would silently mean "already normalised" to the player.
    AudioTags::GainValue parseGain(const QString& raw)
    {
        AudioTags::GainValue g;
        QString text = raw.trimmed();
        if (text.endsWith(QStringLiteral("dB"), Qt::CaseInsensitive))
            text = text.left(text.size() - 2).trimmed();
        if (text.isEmpty())
            return g;
        bool ok = false;
        const double v = text.toDouble(&ok);
        if (!ok)
            return g;
        g.present = true;
        g.value   = v;
        return g;
    }

    // Containers name the picture's type differently and MP4 does not name it at all, so sniff the encoding
    // from the bytes when the tag's own mime type is missing or junk. A cover with no usable mime is a cover
    // the UI cannot hand to an image loader with a hint, and guessing from the extension is not available —
    // the bytes are all we have.
    QString sniffMime(const QByteArray& data)
    {
        if (data.startsWith("\xFF\xD8\xFF"))
            return QStringLiteral("image/jpeg");
        if (data.startsWith(QByteArray("\x89PNG\r\n\x1A\n", 8)))
            return QStringLiteral("image/png");
        if (data.startsWith("GIF8"))
            return QStringLiteral("image/gif");
        if (data.size() > 12 && data.startsWith("RIFF") && data.mid(8, 4) == "WEBP")
            return QStringLiteral("image/webp");
        if (data.startsWith("BM"))
            return QStringLiteral("image/bmp");
        return QString();
    }

    // The cover, preferring the FRONT one. A tagged rip routinely carries several pictures (front, back,
    // media, artist), they arrive in file order, and taking the first one gets an album shelf full of CD
    // undersides. Type names come from TagLib's own picture-type strings ("Front Cover", "Other", ...).
    AudioTags::Picture pickCover(const TagLib::List<TagLib::VariantMap>& pictures)
    {
        AudioTags::Picture best;
        bool bestIsFront = false;

        for (const auto& p : pictures)
        {
            const TagLib::ByteVector bytes = p.value("data").toByteVector();
            if (bytes.isEmpty())
                continue;

            AudioTags::Picture candidate;
            candidate.data     = QByteArray(bytes.data(), int(bytes.size()));
            candidate.mimeType = qstr(p.value("mimeType").toString()).trimmed();
            if (!candidate.mimeType.startsWith(QStringLiteral("image/")))
                candidate.mimeType = sniffMime(candidate.data);

            const bool isFront =
                qstr(p.value("pictureType").toString()).compare(QStringLiteral("Front Cover"), Qt::CaseInsensitive) == 0;
            if (best.isNull() || (isFront && !bestIsFront))
            {
                best        = candidate;
                bestIsFront = isFront;
            }
        }
        return best;
    }
}

namespace AudioTags
{
    Tags read(const QString& filePath)
    {
        Tags tags;
        if (filePath.isEmpty())
            return tags;

        // BELT AND BRACES, AND MEASURED AS SUCH: mutation-testing this branch away leaves the probe green
        // (SURVIVED), because on Windows TagLib's own fopen refuses a missing path and a directory alike and
        // the isValid() guard below catches both. It stays because that equivalence is a PLATFORM fact, not a
        // TagLib promise — glibc's fopen opens a directory quite happily, and a scan hands this function
        // every entry it walks past. Refusing here makes "not a file" the same answer everywhere rather than
        // one that depends on the C runtime underneath.
        const QFileInfo info(filePath);
        if (!info.exists() || !info.isFile())
            return tags;

#ifdef _WIN32
        // TagLib's FileName takes a wchar_t* on Windows and a byte path elsewhere. The wide path is not
        // optional: a byte path goes through the ANSI code page, so a track under a folder with a non-Latin
        // name simply fails to open — the exact files whose tags matter most.
        const std::wstring native = filePath.toStdWString();
        TagLib::FileRef ref(TagLib::FileName(native.c_str()), true, TagLib::AudioProperties::Average);
#else
        const QByteArray native = QFile::encodeName(filePath);
        TagLib::FileRef ref(TagLib::FileName(native.constData()), true, TagLib::AudioProperties::Average);
#endif
        // isNull covers "no reader for this container"; isValid covers "the reader opened it and found the
        // container broken" (a truncated FLAC, an mp4 with no moov). Past this point TagLib's tag pointers are
        // safe to touch; before it they are not, and asking an invalid file for its properties is a crash.
        if (ref.isNull() || ref.file() == nullptr || !ref.file()->isValid())
            return tags;

        const QHash<QString, QString> props = foldedProperties(ref.file()->properties());

        tags.title       = value(props, "TITLE");
        tags.artist      = value(props, "ARTIST");
        tags.albumArtist = value(props, "ALBUMARTIST");
        tags.album       = value(props, "ALBUM");
        tags.genre       = value(props, "GENRE");

        parsePair(value(props, "TRACKNUMBER"), tags.track, tags.trackTotal);
        parsePair(value(props, "DISCNUMBER"), tags.disc, tags.discTotal);

        // DATE is what every container maps its year-ish field to (ID3v2 TDRC/TYER, Vorbis DATE, MP4 ©day).
        // ORIGINALDATE is the fallback for a re-release tagged with the recording's own year and nothing else.
        tags.year = parseYear(value(props, "DATE"));
        if (tags.year == 0)
            tags.year = parseYear(value(props, "ORIGINALDATE"));

        tags.trackGain = parseGain(value(props, "REPLAYGAIN_TRACK_GAIN"));
        tags.albumGain = parseGain(value(props, "REPLAYGAIN_ALBUM_GAIN"));
        tags.trackPeak = parseGain(value(props, "REPLAYGAIN_TRACK_PEAK"));
        tags.albumPeak = parseGain(value(props, "REPLAYGAIN_ALBUM_PEAK"));

        tags.cover = pickCover(ref.file()->complexProperties("PICTURE"));

        // Embedded lyrics (issue #142, source 2) — out of the SAME read, per the issue. The text tag comes
        // from the property map we already folded; the structurally-timed SYLT frame does not appear there at
        // all and is fetched from the ID3v2 tag directly.
        tags.lyrics       = lyricsValue(props);
        tags.syncedLyrics = readSylt(ref.file());

        if (const TagLib::AudioProperties* audio = ref.audioProperties())
            tags.durationSec = audio->lengthInSeconds() > 0 ? audio->lengthInSeconds() : 0;

        return tags;
    }

    bool isSupportedFile(const QString& filePath)
    {
        // The containers TagLib is built with here that a music library plausibly meets. Deliberately a fixed
        // set rather than "anything TagLib might open": the scan uses this to decide what to even look at, and
        // a video container that happens to carry a readable tag block is not a music file.
        static const QSet<QString> kSuffixes = {
            QStringLiteral("mp3"),  QStringLiteral("flac"), QStringLiteral("ogg"),  QStringLiteral("oga"),
            QStringLiteral("opus"), QStringLiteral("spx"),  QStringLiteral("m4a"),  QStringLiteral("m4b"),
            QStringLiteral("mp4a"), QStringLiteral("aac"),  QStringLiteral("wav"),  QStringLiteral("aiff"),
            QStringLiteral("aif"),  QStringLiteral("aifc"), QStringLiteral("wma"),  QStringLiteral("ape"),
            QStringLiteral("wv"),   QStringLiteral("mpc"),  QStringLiteral("tta"),  QStringLiteral("dsf"),
            QStringLiteral("dff")
        };
        return kSuffixes.contains(QFileInfo(filePath).suffix().toLower());
    }
}
