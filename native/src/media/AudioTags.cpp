#include "AudioTags.h"

#include "LrcLyrics.h" // SYLT is rendered back to LRC text here, so the app keeps ONE lyric parser (#142)

#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
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
// CHAP frames (issue #139) live outside the property map for the same reason SYLT does, so the ID3v2 side is
// reached the same way. The MP4 headers are here for the narrator's own `©nrt` atom, which TagLib's property
// table does not know and therefore drops — see mp4Narrator below.
#include <chapterframe.h>
#include <mp4file.h>
#include <mp4item.h>
#include <mp4tag.h>

#include <algorithm>
#include <limits>
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
    // First value only, for the fields that are genuinely one thing (title, album, year, ReplayGain…). The
    // two fields that are NOT — ARTIST and GENRE — go through multiValue() below instead, which is where
    // #196's structured multi-value reading happens; a repeated Vorbis field or a NUL-separated ID3v2.4
    // frame arrives here as a StringList and this function would silently keep only its head.
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

    // EVERY value TagLib parsed for one key, in tag order — the structured half of #196. The property map is
    // walked directly rather than through foldedProperties() because that one flattens to a single string by
    // design, and this is the one question it cannot answer. Keys are uppercased for the same reason they are
    // there (see foldedProperties), and the FIRST matching key wins so a file carrying both "ARTIST" and a
    // freeform "artist" cannot double up.
    QStringList multiValue(const TagLib::PropertyMap& props, const char* key)
    {
        const QString want = QString::fromLatin1(key);
        for (const auto& entry : props)
        {
            if (qstr(entry.first).toUpper() != want)
                continue;
            QStringList out;
            for (const auto& v : entry.second)
            {
                const QString s = qstr(v).trimmed();
                if (!s.isEmpty()) out << s;
            }
            if (!out.isEmpty()) return out;
        }
        return {};
    }

    // Case-insensitive de-duplication, first spelling kept. "A; a" is one artist, not two rows in a browse —
    // the same fold MusicLibrary applies to its grouping keys, applied here so the list a caller receives is
    // already the list it should show.
    QStringList dedupe(const QStringList& in)
    {
        QStringList out;
        QSet<QString> seen;
        for (const QString& s : in)
        {
            const QString k = s.toCaseFolded();
            if (seen.contains(k)) continue;
            seen.insert(k);
            out << s;
        }
        return out;
    }

    // The ad-hoc separators as ONE alternation. Built per call rather than cached because the list is a
    // user setting, not a constant — and a scan pays for it once per file, next to a disk read.
    //
    // A separator containing a letter is anchored to whitespace on BOTH sides. That is the rule that keeps
    // "feat." from cutting "Featherstone" and "and" from cutting "Bandwagon": those are not separators
    // there, they are spelling. Pure punctuation is matched literally, because ";" is never spelling — and
    // because anchoring it would miss "A;B", which is exactly how the taggers that write it write it.
    QRegularExpression separatorPattern(const QStringList& separators)
    {
        QStringList alts;
        for (const QString& sepRaw : separators)
        {
            const QString sep = sepRaw.trimmed();
            if (sep.isEmpty()) continue;
            bool hasLetter = false;
            for (const QChar& c : sep) if (c.isLetter()) { hasLetter = true; break; }
            const QString lit = QRegularExpression::escape(sep);
            alts << (hasLetter ? QStringLiteral("(?<=\\s)%1(?=\\s)").arg(lit) : lit);
        }
        if (alts.isEmpty()) return QRegularExpression();
        return QRegularExpression(alts.join(QChar('|')), QRegularExpression::CaseInsensitiveOption);
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

    // ------------------------------------------------------------------------------------------------
    // CHAPTERS (issue #139). The ONE thing in this file that is not a tag-block read, which is why it is
    // reached only when a caller asks for it — see AudioTags.h's `withChapters`.
    //
    // WHY IT IS NOT IN THE PROPERTY MAP. A chapter list is not a tag: it is either a `chpl` atom in an MP4's
    // udta, or one CHAP frame per chapter in an ID3v2 tag. TagLib exposes the second and nothing at all of
    // the first, so the MP4 side walks the atom tree here and the mp3 side asks TagLib.
    // ------------------------------------------------------------------------------------------------
    quint32 be32(const QByteArray& b, int at)
    {
        return (quint32(quint8(b.at(at))) << 24) | (quint32(quint8(b.at(at + 1))) << 16)
             | (quint32(quint8(b.at(at + 2))) << 8) | quint32(quint8(b.at(at + 3)));
    }

    quint64 be64(const QByteArray& b, int at)
    {
        return (quint64(be32(b, at)) << 32) | quint64(be32(b, at + 4));
    }

    // Find the first child atom named `name` between [begin, end) and hand back ITS BODY's range. The walk
    // is deliberately paranoid about sizes: a scan meets truncated downloads and files that are not what
    // their extension says, and an atom whose declared size is smaller than its own header would otherwise
    // spin this loop forever on somebody's disk.
    bool findAtom(QFile& f, qint64 begin, qint64 end, const char* name, qint64& bodyBegin, qint64& bodyEnd)
    {
        qint64 p = begin;
        while (p + 8 <= end)
        {
            if (!f.seek(p)) return false;
            const QByteArray hdr = f.read(8);
            if (hdr.size() < 8) return false;
            qint64 size   = qint64(be32(hdr, 0));
            qint64 hdrLen = 8;
            if (size == 1)
            {
                const QByteArray ext = f.read(8);
                if (ext.size() < 8) return false;
                const quint64 wide = be64(ext, 0);
                if (wide > quint64(std::numeric_limits<qint64>::max())) return false;
                size   = qint64(wide);
                hdrLen = 16;
            }
            else if (size == 0)
            {
                size = end - p;            // "to the end of the enclosing box", per the spec
            }
            if (size < hdrLen || p + size > end) return false;
            if (hdr.mid(4, 4) == QByteArray(name, 4))
            {
                bodyBegin = p + hdrLen;
                bodyEnd   = p + size;
                return true;
            }
            p += size;
        }
        return false;
    }

    // The NERO chapter list — moov/udta/chpl — which is what every m4b writer that matters emits (mp4v2's
    // mp4chaps, ffmpeg, and the tools built on them). Layout, as ffmpeg's mov_read_chpl reads it and as
    // mp4v2 writes it: one version byte, three flag bytes, FOUR RESERVED BYTES WHEN THE VERSION IS NON-ZERO,
    // a ONE-BYTE chapter count, then per chapter an 8-byte big-endian start in 100-nanosecond units and a
    // Pascal string (one length byte, then UTF-8).
    //
    // KNOWN GAP, stated rather than hidden: an MP4 whose chapters are ONLY a QuickTime chapter TEXT TRACK (a
    // trak referenced by tref/chap) reports none here. Reading that means walking a sample table — stts,
    // stsc, stsz, stco — to find the text samples, which is a materially larger parser than this one; #139's
    // increment 1 reads the atom the issue calls cheap and leaves the track for a follow-up. Nothing breaks
    // in that case: the book simply has no chapter count, and mpv still shows its chapters at play time.
    QVector<AudioTags::Chapter> readMp4Chapters(const QString& path)
    {
        QVector<AudioTags::Chapter> out;
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) return out;
        const qint64 end = f.size();

        qint64 moovB = 0, moovE = 0, udtaB = 0, udtaE = 0, chplB = 0, chplE = 0;
        if (!findAtom(f, 0, end, "moov", moovB, moovE)) return out;
        if (!findAtom(f, moovB, moovE, "udta", udtaB, udtaE)) return out;
        if (!findAtom(f, udtaB, udtaE, "chpl", chplB, chplE)) return out;

        const qint64 len = chplE - chplB;
        if (len < 5 || len > (4 << 20)) return out;   // a chapter list is kilobytes; anything else is garbage
        if (!f.seek(chplB)) return out;
        const QByteArray body = f.read(len);
        if (body.size() != len) return out;

        int at = 0;
        const quint8 version = quint8(body.at(0));
        at += 4;                                   // version + flags
        if (version != 0) at += 4;                 // the reserved word only a versioned chpl carries
        if (at >= body.size()) return out;
        const int count = quint8(body.at(at));
        ++at;
        for (int i = 0; i < count; ++i)
        {
            if (at + 9 > body.size()) break;       // truncated list: keep the chapters we did read
            const quint64 start100ns = be64(body, at);
            at += 8;
            const int titleLen = quint8(body.at(at));
            ++at;
            if (at + titleLen > body.size()) break;
            AudioTags::Chapter c;
            c.title   = QString::fromUtf8(body.constData() + at, titleLen).trimmed();
            c.startMs = int(qMin<quint64>(start100ns / 10000ull, quint64(std::numeric_limits<int>::max())));
            at += titleLen;
            out.push_back(c);
        }
        return out;
    }

    // ID3v2 CHAP frames — the mp3 half, and free: TagLib has already parsed the tag we are holding, so this
    // is a walk of frames in memory rather than a second read of the file. A chaptered mp3 is how a great
    // many single-file audiobooks and every chaptered podcast episode are actually shipped.
    QVector<AudioTags::Chapter> readId3Chapters(TagLib::File* file)
    {
        QVector<AudioTags::Chapter> out;
        auto* mpeg = dynamic_cast<TagLib::MPEG::File*>(file);
        if (!mpeg || !mpeg->hasID3v2Tag()) return out;
        const TagLib::ID3v2::FrameList frames = mpeg->ID3v2Tag()->frameList("CHAP");
        for (const auto* frame : frames)
        {
            const auto* chap = dynamic_cast<const TagLib::ID3v2::ChapterFrame*>(frame);
            if (!chap) continue;
            AudioTags::Chapter c;
            c.startMs = int(qMin<quint32>(chap->startTime(), quint32(std::numeric_limits<int>::max())));
            // The chapter's NAME is an embedded TIT2 sub-frame, not a field on the frame. A chapter with no
            // sub-frame keeps an empty title, which the display layer numbers.
            const TagLib::ID3v2::FrameList& subs = chap->embeddedFrameList("TIT2");
            if (!subs.isEmpty()) c.title = qstr(subs.front()->toString()).trimmed();
            out.push_back(c);
        }
        // CHAP frames carry their order in their start time, and a tag is free to store them in any order at
        // all. Sorting by start is what makes "chapter 3" mean the third one you hear.
        std::sort(out.begin(), out.end(), [](const AudioTags::Chapter& a, const AudioTags::Chapter& b) {
            return a.startMs < b.startMs;
        });
        return out;
    }

    // The NARRATOR's own atom (issue #139). `©nrt` is not in TagLib's MP4 property table — a four-character
    // name it does not know becomes "unsupported data" with its VALUE dropped — so the one container that
    // has a dedicated narrator field is reached directly. A freeform "----:com.apple.iTunes:NARRATOR" needs
    // none of this: TagLib turns any freeform name into a property key, so that spelling arrives in the map.
    QString mp4Narrator(TagLib::File* file)
    {
        auto* mp4 = dynamic_cast<TagLib::MP4::File*>(file);
        if (!mp4 || !mp4->tag()) return {};
        const TagLib::MP4::Item item = mp4->tag()->item("\251nrt");
        if (!item.isValid()) return {};
        const TagLib::StringList values = item.toStringList();
        return values.isEmpty() ? QString() : qstr(values.front()).trimmed();
    }
}

namespace AudioTags
{
    QStringList splitTagValues(const QString& raw, const QStringList& separators)
    {
        const QString text = raw.trimmed();
        if (text.isEmpty())
            return {};

        const QRegularExpression re = separatorPattern(separators);
        if (!re.isValid() || re.pattern().isEmpty())
            return { text };                       // no separators configured: the string is one value

        QStringList parts;
        for (const QString& p : text.split(re))
        {
            const QString t = p.trimmed();
            if (!t.isEmpty()) parts << t;
        }
        // A value that is nothing but separators ("///") splits to nothing, and returning nothing would
        // silently delete a credit. Give the original back instead: unreadable, but present and browsable.
        if (parts.isEmpty())
            return { text };
        return dedupe(parts);
    }

    namespace
    {
        // The two-step rule from the header, applied to one field. Structured values are taken as they are —
        // the container already answered, and splitting its answer again could only invent artists that a
        // deliberate "A/B" Vorbis comment never meant. Only a single value falls through to the ad-hoc list.
        QStringList valuesFor(const TagLib::PropertyMap& props, const char* key, const QStringList& separators)
        {
            const QStringList structured = multiValue(props, key);
            if (structured.size() > 1) return dedupe(structured);
            if (structured.size() == 1) return splitTagValues(structured.first(), separators);
            return {};
        }

        // The one display string for a multi-valued field. Joined with "; " rather than with whatever the
        // file used, because when the container carried the values STRUCTURALLY it used no separator at all
        // and there is nothing to preserve — and because "; " is the app's own default, so what a person
        // reads is a string this reader would parse back into the same list.
        QString displayOf(const QStringList& values, const QString& singleFallback)
        {
            if (values.isEmpty()) return singleFallback;
            if (values.size() == 1) return values.first();
            return values.join(QStringLiteral("; "));
        }

        // PERFORMER (#196, part 2) — the one classical field with no single spelling across containers, so
        // it gets a reader of its own rather than another valuesFor() call.
        //
        // A Vorbis comment and an MP4 freeform atom both carry a plain PERFORMER, and that is the first
        // half. ID3v2 has NO plain performer frame: players live in TMCL, one entry per INSTRUMENT, which
        // TagLib folds into "PERFORMER:<instrument>" keys ("PERFORMER:violin", "PERFORMER:orchestra"). The
        // instrument-keyed entries are gathered too, and the instrument is DROPPED — the browse dimension
        // the issue asks for is the person, not the desk they sit at, and a library that filed "violin" and
        // "viola" as two performers would be unreadable. Without this half every classical mp3 in the world
        // reports no performers at all, which is the same as not having read the field.
        //
        // Order is "plain first, then instrument-keyed in tag order", and dedupe() keeps the first spelling,
        // so a player credited both ways appears once.
        QStringList performerValues(const TagLib::PropertyMap& props, const QStringList& separators)
        {
            QStringList out = valuesFor(props, "PERFORMER", separators);
            static const QString kPrefix = QStringLiteral("PERFORMER:");
            for (const auto& entry : props)
            {
                if (!qstr(entry.first).toUpper().startsWith(kPrefix))
                    continue;
                for (const auto& v : entry.second)
                    out += splitTagValues(qstr(v).trimmed(), separators);   // empty in, nothing out
            }
            return dedupe(out);
        }
    }

    Tags read(const QString& filePath, const QStringList& separators, bool withChapters)
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

        const TagLib::PropertyMap raw   = ref.file()->properties();
        const QHash<QString, QString> props = foldedProperties(raw);

        tags.title       = value(props, "TITLE");
        tags.albumArtist = value(props, "ALBUMARTIST");   // SINGLE-VALUED: never split (see the header)
        tags.album       = value(props, "ALBUM");

        // The two multi-valued fields (#196). The lists are authoritative and the strings are their display.
        tags.artists = valuesFor(raw, "ARTIST", separators);
        tags.genres  = valuesFor(raw, "GENRE", separators);
        tags.artist  = displayOf(tags.artists, value(props, "ARTIST"));
        tags.genre   = displayOf(tags.genres, value(props, "GENRE"));

        // The classical fields (#196, part 2), out of the SAME property map and the same pass. Multi-valued
        // by the same two-step rule, because a concerto credits two soloists as routinely as a pop track
        // credits two rappers.
        tags.composers  = valuesFor(raw, "COMPOSER", separators);
        tags.conductors = valuesFor(raw, "CONDUCTOR", separators);
        tags.performers = performerValues(raw, separators);
        tags.composer   = displayOf(tags.composers, value(props, "COMPOSER"));
        tags.conductor  = displayOf(tags.conductors, value(props, "CONDUCTOR"));
        tags.performer  = displayOf(tags.performers, value(props, "PERFORMER"));

        // SINGLE-VALUED: one track, one work, one movement. MOVEMENTNAME is what TagLib normalises ID3v2's
        // MVNM and MP4's ©mvn to and what Picard writes into a Vorbis comment; a bare MOVEMENT is the older
        // spelling and is taken only when the modern one is absent, so a file carrying both is read once.
        tags.work     = value(props, "WORK");
        tags.movement = value(props, "MOVEMENTNAME");
        if (tags.movement.isEmpty())
            tags.movement = value(props, "MOVEMENT");

        // MusicBrainz ids (#194). TagLib normalises Picard's spelling for every container it knows —
        // an ID3v2 TXXX description, an MP4 "----:com.apple.iTunes:…" freeform atom and a Vorbis comment all
        // arrive as MUSICBRAINZ_ALBUMID — but a file written by an older tagger (or by a tool that wrote the
        // TXXX description verbatim) still carries the SPACED spelling, and foldedProperties uppercases
        // whatever it was given. Both are read, the normalised one first, because a file that carries both
        // must be read once and the same way every run.
        const auto mb = [&props](const char* modern, const char* spaced) {
            const QString v = value(props, modern);
            return v.isEmpty() ? value(props, spaced) : v;
        };
        tags.mbReleaseGroupId = mb("MUSICBRAINZ_RELEASEGROUPID", "MUSICBRAINZ RELEASE GROUP ID");
        tags.mbReleaseId      = mb("MUSICBRAINZ_ALBUMID",        "MUSICBRAINZ ALBUM ID");
        tags.mbAlbumArtistId  = mb("MUSICBRAINZ_ALBUMARTISTID",  "MUSICBRAINZ ALBUM ARTIST ID");

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

        // The embedded cue sheet (issue #196, part 3) — the sidecar's other half, out of the same read. Not
        // parsed here: this is a tag reader, and CueSheet owns what a sheet means.
        tags.cuesheet = value(props, "CUESHEET");

        // ---- The audiobook fields (issue #139), out of the same map and the same pass ----------------
        // NARRATOR. The explicit tag first — a freeform NARRATOR arrives in the property map, and MP4's own
        // `©nrt` atom does not, so it is fetched directly. NOT falling back to COMPOSER here, and that is
        // the point: this reader must not decide that a composer is a narrator. AudiobookLibrary applies
        // that fallback, because only a file's ROOT can say which of the two the tag means (see the header).
        tags.narrator = value(props, "NARRATOR");
        if (tags.narrator.isEmpty()) tags.narrator = mp4Narrator(ref.file());

        // SERIES + index. One property key covers all three containers: TagLib turns a Vorbis SERIES
        // comment, an ID3v2 TXXX:SERIES frame and an MP4 "----:com.apple.iTunes:SERIES" atom into the same
        // "SERIES". MOVEMENTNAME/MOVEMENTNUMBER is the fallback, which is the Apple Books spelling (MVNM and
        // ©mvn) and the one thing Mp3tag's audiobook presets write. `movement` above reads the same tag for
        // classical music, and both readings are honest: which is meant is decided by the library ROOT.
        tags.series = value(props, "SERIES");
        if (tags.series.isEmpty()) tags.series = value(props, "MOVEMENTNAME");
        {
            // "3", "3/14" and "Book 3" all appear in the wild; parsePair handles the first two and the
            // trailing-number fallback handles the third without inventing an index for prose.
            int total = 0;
            QString raw = value(props, "SERIES-PART");
            if (raw.isEmpty()) raw = value(props, "SERIESPART");
            if (raw.isEmpty()) raw = value(props, "MOVEMENTNUMBER");
            parsePair(raw, tags.seriesIndex, total);
            if (tags.seriesIndex == 0)
            {
                static const QRegularExpression trailing(QStringLiteral("(\\d+)\\s*$"));
                const QRegularExpressionMatch m = trailing.match(raw);
                if (m.hasMatch()) tags.seriesIndex = m.captured(1).toInt();
            }
        }

        // CHAPTERS, only when the caller asked (see AudioTags.h). MP4 first because that is where an m4b
        // keeps them; the ID3v2 walk costs nothing on a file with no CHAP frames and is what a chaptered
        // mp3 needs. Anything else reports none, which is the honest answer rather than a guessed one.
        if (withChapters)
        {
            tags.chapters = readMp4Chapters(filePath);
            if (tags.chapters.isEmpty()) tags.chapters = readId3Chapters(ref.file());
        }

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
