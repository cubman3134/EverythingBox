#include "CueSheet.h"

#include <QFile>
#include <QStringDecoder>

namespace CueSheet
{
namespace
{
    // A cue line is "<COMMAND> <arguments>". The command is the first whitespace-run-delimited word; what
    // follows depends on the command, so it is handed back raw rather than pre-split — an unquoted TITLE runs
    // to the end of the line and pre-splitting it would shred every album title with a space in it.
    int firstSpace(const QString& s, int from = 0)
    {
        for (int i = from; i < s.size(); ++i) if (s.at(i).isSpace()) return i;
        return -1;
    }

    int lastSpace(const QString& s)
    {
        for (int i = s.size() - 1; i >= 0; --i) if (s.at(i).isSpace()) return i;
        return -1;
    }

    QString takeWord(const QString& line, QString* rest)
    {
        const QString trimmed = line.trimmed();
        const int sp = firstSpace(trimmed);
        if (sp < 0) { if (rest) rest->clear(); return trimmed; }
        if (rest) *rest = trimmed.mid(sp + 1).trimmed();
        return trimmed.left(sp);
    }

    // The value of a TITLE / PERFORMER / SONGWRITER / FILE argument: the contents of the double quotes when
    // there are any, otherwise everything that is left. `tail` gets what followed a quoted value (the FILE
    // format word, and nothing else in practice) and is empty for an unquoted one.
    //
    // An UNTERMINATED quote takes the rest of the line rather than being refused: a sheet whose title lost
    // its closing quote is a sheet with a slightly odd title, not a sheet whose tracks we should throw away.
    QString quotedValue(const QString& rest, QString* tail = nullptr)
    {
        if (tail) tail->clear();
        if (!rest.startsWith(QLatin1Char('"')))
            return rest.trimmed();
        const int close = rest.indexOf(QLatin1Char('"'), 1);
        if (close < 0) return rest.mid(1).trimmed();
        if (tail) *tail = rest.mid(close + 1).trimmed();
        return rest.mid(1, close - 1);
    }

    // The FILE line's trailing format word ("WAVE", "MP3", "BINARY", …). Dropped from an UNQUOTED file name
    // so `FILE Album.wav WAVE` does not name a file called "Album.wav WAVE"; a quoted name never needs this
    // because the quotes already said where the name ended.
    bool isFileFormatWord(const QString& w)
    {
        static const QStringList kWords = { QStringLiteral("BINARY"), QStringLiteral("MOTOROLA"),
                                            QStringLiteral("AIFF"),   QStringLiteral("WAVE"),
                                            QStringLiteral("MP3"),    QStringLiteral("FLAC") };
        return kWords.contains(w.toUpper());
    }

    // The last component of a path written by somebody else's ripping software: '/' and '\' both count as
    // separators regardless of which platform is reading, because a sheet written on Windows is routinely
    // read on Linux and the other way round.
    QString lastPathComponent(const QString& s)
    {
        int cut = -1;
        for (int i = 0; i < s.size(); ++i)
            if (s.at(i) == QLatin1Char('/') || s.at(i) == QLatin1Char('\\')) cut = i;
        return cut < 0 ? s : s.mid(cut + 1);
    }

    QString baseName(const QString& fileName)
    {
        const int dot = fileName.lastIndexOf(QLatin1Char('.'));
        return dot <= 0 ? fileName : fileName.left(dot);
    }

    bool namesOne(const QString& fileEntry, const QString& audioFileName)
    {
        const QString a = lastPathComponent(fileEntry).trimmed();
        if (a.isEmpty() || audioFileName.isEmpty()) return false;
        if (a.compare(audioFileName, Qt::CaseInsensitive) == 0) return true;
        // The extension may legitimately differ: a rip transcoded from the WAV its sheet still names is the
        // single commonest inconsistency in real cue sheets. The BASE name is what identifies the rip.
        const QString ab = baseName(a), bb = baseName(audioFileName);
        return !ab.isEmpty() && ab.compare(bb, Qt::CaseInsensitive) == 0;
    }
}

int msFromTimecode(const QString& text, bool* ok)
{
    if (ok) *ok = false;
    const QStringList parts = text.trimmed().split(QLatin1Char(':'));
    if (parts.size() != 2 && parts.size() != 3) return 0;

    int v[3] = { 0, 0, 0 };
    for (int i = 0; i < parts.size(); ++i)
    {
        const QString p = parts.at(i);
        if (p.isEmpty() || p.size() > 6) return 0;
        for (const QChar c : p) if (!c.isDigit()) return 0;   // no signs, no spaces, no "01." — digits only
        v[i] = p.toInt();
    }

    const int mm = v[0];
    const int ss = v[1];
    const int ff = parts.size() == 3 ? v[2] : 0;
    if (ss > 59) return 0;                    // a minute holds 60 seconds; a sheet saying otherwise is broken
    if (ff >= kFramesPerSecond) return 0;     // a second holds frames 0..74 — see the header
    // Guards the multiply below from overflowing a signed int: 30000 minutes is 500 hours, longer than any
    // album that has ever been pressed, and 30000*60*1000 still has a third of the range to spare.
    if (mm > 30000) return 0;

    if (ok) *ok = true;
    // FRAMES to milliseconds, rounded to nearest: ff/75 s == ff*1000/75 ms, and the +37 is the half-frame
    // that turns the truncation into a round (frame 1 is 13.33 ms and must read 13; frame 2 is 26.67 and
    // must read 27). Reading FF as hundredths or as milliseconds instead is the silent bug the header names.
    return (mm * 60 + ss) * 1000 + (ff * 1000 + kFramesPerSecond / 2) / kFramesPerSecond;
}

int Sheet::trackCount() const
{
    int n = 0;
    for (const File& f : files) n += int(f.tracks.size());
    return n;
}

bool Sheet::isValid() const
{
    if (files.isEmpty()) return false;
    if (trackCount() == 0) return false;
    for (const File& f : files)
    {
        // ONE rule, covering two failures that are the same failure: a track nobody could place, and a track
        // that starts at or before the one in front of it. `prev` STARTS AT -1 ON PURPOSE and that is
        // load-bearing rather than decorative — an unplaced track is -1, so it fails this comparison without
        // needing a clause of its own, and the FIRST track is caught by it as surely as the fifth. Equal
        // starts are refused too: that is a track of zero length, which is a broken sheet, not a short song.
        int prev = -1;
        for (const Track& t : f.tracks)
        {
            if (t.startMs <= prev) return false;
            prev = t.startMs;
        }
    }
    return true;
}

Sheet parse(const QString& text)
{
    Sheet s;
    // CRLF, LF and lone-CR all appear in sheets written over the last thirty years, and a sheet read as one
    // enormous line would parse as nothing at all.
    QString norm = text;
    norm.replace(QLatin1String("\r\n"), QLatin1String("\n"));
    norm.replace(QLatin1Char('\r'), QLatin1Char('\n'));

    File* file = nullptr;          // the FILE section we are inside, if any
    Track* track = nullptr;        // the TRACK we are inside, if any
    bool skippingTrack = false;    // inside a non-AUDIO track: its INDEX lines belong to nobody

    for (const QString& raw : norm.split(QLatin1Char('\n')))
    {
        QString rest;
        const QString cmd = takeWord(raw, &rest).toUpper();
        if (cmd.isEmpty()) continue;

        if (cmd == QLatin1String("FILE"))
        {
            QString tail;
            QString name = quotedValue(rest, &tail);
            if (tail.isEmpty() && !rest.startsWith(QLatin1Char('"')))
            {
                // Unquoted: the last word may be the format ("Album.wav WAVE"). Only drop it if it looks
                // like one — a file genuinely called "Live WAVE.flac" keeps its name.
                const int sp = lastSpace(name);
                if (sp > 0 && isFileFormatWord(name.mid(sp + 1))) name = name.left(sp).trimmed();
            }
            File f;
            f.name = name;
            s.files.push_back(f);
            file = &s.files.last();
            track = nullptr;
            skippingTrack = false;
            continue;
        }

        if (cmd == QLatin1String("TRACK"))
        {
            track = nullptr;
            skippingTrack = true;
            // A TRACK before any FILE belongs to nothing; it is dropped rather than being attached to a
            // file that does not exist. The sheet then has fewer tracks than it looks like it has, which
            // singleFileSegments answers with "this cue changes nothing".
            if (!file) continue;
            QString kind;
            const QString num = takeWord(rest, &kind);
            // AUDIO only. A mixed-mode disc's MODE1/2352 track is a filesystem, not music, and expanding it
            // into the album would add a track that plays nothing.
            if (kind.trimmed().toUpper() != QLatin1String("AUDIO")) continue;
            Track t;
            t.number = num.toInt();
            file->tracks.push_back(t);
            track = &file->tracks.last();
            skippingTrack = false;
            continue;
        }

        if (cmd == QLatin1String("INDEX"))
        {
            if (skippingTrack || !track) continue;
            QString stamp;
            const int which = takeWord(rest, &stamp).toInt();
            bool ok = false;
            const int ms = msFromTimecode(stamp, &ok);
            if (!ok) continue;                 // leaves startMs at -1 for INDEX 01 -> the sheet is invalid
            if (which == 1) { track->startMs = ms; }
            else if (which == 0)
            {
                track->pregapMs = ms;
                // Only as a FALLBACK. A sheet that writes a pregap and no INDEX 01 still places its track;
                // one that writes both is placed by the INDEX 01, which is where the music starts.
                if (track->startMs < 0) track->startMs = ms;
            }
            continue;
        }

        if (cmd == QLatin1String("TITLE"))
        {
            const QString v = quotedValue(rest);
            if (track) track->title = v; else s.title = v;
            continue;
        }
        if (cmd == QLatin1String("PERFORMER"))
        {
            const QString v = quotedValue(rest);
            if (track) track->performer = v; else s.performer = v;
            continue;
        }
        if (cmd == QLatin1String("SONGWRITER"))
        {
            const QString v = quotedValue(rest);
            if (track) track->songwriter = v; else s.songwriter = v;
            continue;
        }
        if (cmd == QLatin1String("ISRC"))
        {
            if (track) track->isrc = quotedValue(rest);
            continue;
        }
        if (cmd == QLatin1String("REM"))
        {
            // REM is the cue's comment, and every ripper puts its extra metadata in one. Two are worth
            // having; the rest are ignored rather than refused.
            QString value;
            const QString key = takeWord(rest, &value).toUpper();
            if (key == QLatin1String("GENRE")) s.genre = quotedValue(value);
            else if (key == QLatin1String("DATE"))
            {
                const QString d = quotedValue(value);
                bool ok = false;
                const int y = d.left(4).toInt(&ok);
                if (ok && y > 1000 && y < 3000) s.year = y;
            }
            continue;
        }
        // CATALOG, CDTEXTFILE, FLAGS, PREGAP, POSTGAP and whatever a future ripper invents: ignored. The
        // parser is generous about SHAPE and strict about TIME (see the header) — an unknown command tells
        // us nothing we need and is not a reason to refuse an album.
    }
    return s;
}

Sheet parseBytes(const QByteArray& bytes)
{
    if (bytes.isEmpty()) return {};

    // A BOM answers the question outright.
    if (bytes.size() >= 2)
    {
        const uchar b0 = uchar(bytes.at(0)), b1 = uchar(bytes.at(1));
        if (b0 == 0xFF && b1 == 0xFE)
            return parse(QStringDecoder(QStringDecoder::Utf16LE)(bytes.mid(2)));
        if (b0 == 0xFE && b1 == 0xFF)
            return parse(QStringDecoder(QStringDecoder::Utf16BE)(bytes.mid(2)));
    }
    if (bytes.startsWith(QByteArray("\xEF\xBB\xBF", 3)))
        return parse(QString::fromUtf8(bytes.mid(3)));

    // No BOM. STRICT UTF-8 first, because a modern sheet is UTF-8 and mis-reading it as Latin-1 would
    // mojibake every accented name; Latin-1 as the fallback, because it cannot fail and a Windows-1252
    // sheet is then merely spelled oddly rather than refused. (The two agree exactly on pure ASCII, which
    // is the overwhelming majority of sheets, so this decision is invisible for most libraries.)
    QStringDecoder utf8(QStringDecoder::Utf8, QStringDecoder::Flag::Stateless);
    const QString text = utf8(bytes);
    if (!utf8.hasError()) return parse(text);
    return parse(QString::fromLatin1(bytes));
}

Sheet load(const QString& cuePath)
{
    QFile f(cuePath);
    if (!f.open(QIODevice::ReadOnly)) return {};
    // A cue sheet is a few kilobytes. The cap is here so that a file which merely ENDS in .cue — a rename
    // accident, a disk image, an HTML error page saved by a browser — cannot pull a gigabyte into a scan.
    return parseBytes(f.read(4 * 1024 * 1024));
}

bool namesAudioFile(const Sheet& sheet, const QString& audioFileName)
{
    for (const File& f : sheet.files)
        if (namesOne(f.name, audioFileName)) return true;
    return false;
}

QVector<Segment> singleFileSegments(const Sheet& sheet)
{
    QVector<Segment> out;
    if (sheet.files.size() != 1) return out;   // a cue over per-track files: that album is already correct
    if (!sheet.isValid()) return out;
    const File& f = sheet.files.first();
    if (f.tracks.size() < 2) return out;       // one track describes the file we already have

    out.reserve(f.tracks.size());
    for (int i = 0; i < f.tracks.size(); ++i)
    {
        const Track& t = f.tracks.at(i);
        Segment g;
        g.number     = t.number;
        g.title      = t.title.trimmed();
        g.performer  = t.performer.trimmed().isEmpty()  ? sheet.performer.trimmed()  : t.performer.trimmed();
        g.songwriter = t.songwriter.trimmed().isEmpty() ? sheet.songwriter.trimmed() : t.songwriter.trimmed();
        g.startMs    = t.startMs;
        // The boundary is the NEXT track's INDEX 01, so a pregap plays out as the tail of the outgoing
        // track — which is what mpv's own cue demuxer does. The header says why agreeing matters.
        g.endMs      = (i + 1 < f.tracks.size()) ? f.tracks.at(i + 1).startMs : -1;
        out.push_back(g);
    }
    return out;
}

QString mpvClipUrl(const QString& filePath, int startMs, int endMs)
{
    if (filePath.isEmpty()) return {};
    const int start = startMs > 0 ? startMs : 0;
    // BYTES, not QChars: mpv's EDL parser counts the octets it is handed, and the app hands it UTF-8.
    const qsizetype bytes = filePath.toUtf8().size();

    QString url = QStringLiteral("edl://%") + QString::number(bytes) + QLatin1Char('%') + filePath
                + QLatin1Char(',') + QString::number(start / 1000.0, 'f', 3);
    // A length only when there is a real one. Omitted means "to the end of the file", which is the last
    // track; a faked length would be reported as the duration and the progress bar would lie.
    if (endMs > start)
        url += QLatin1Char(',') + QString::number((endMs - start) / 1000.0, 'f', 3);
    return url + QLatin1Char(';');
}

} // namespace CueSheet
