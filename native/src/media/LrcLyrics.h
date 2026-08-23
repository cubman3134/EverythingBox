// The LRC lyric PARSER + the current-line lookup, pulled out of the player so both can be pinned without a
// window, a file on disk or a running clock (issue #142, source 1 — the LRC sidecar). Deliberately pure:
// QtCore only, no file I/O, no network, no keys. The sidecar load (open <basename>.lrc beside the audio file,
// read it, hand the text here) and the karaoke render (NowPlayingAudio.qml) are thin shells over these two.
//
// LRC IS ALSO THE INTERCHANGE FORMAT FOR THE OTHER TWO SOURCES (#142). Embedded tags and LRCLIB do not carry
// a third and fourth lyric model: an ID3v2 SYLT frame is rendered back to LRC text by renderLrc() below, a
// USLT / MP4 ©lyr / Vorbis LYRICS value is handed here verbatim (a large minority of them are LRC already),
// and LRCLIB's syncedLyrics field is LRC by definition. Everything therefore lands on ONE parser, and the
// synced-vs-unsynced decision is made in exactly one place — here, by whether a timestamp was found.
//
// The LRC format this parses (the de-facto community standard):
//   * a timestamp tag [mm:ss.xx] (also [mm:ss.xxx] and bare [mm:ss]) prefixes a lyric line;
//   * MULTIPLE timestamps may prefix ONE line — [00:12.00][00:47.00]Chorus means the SAME text sung at both
//     times, so it becomes two entries;
//   * ID tags [ti:], [ar:], [al:] fill title/artist/album; [offset:±ms] nudges EVERY line time;
//   * enhanced word-level tags <mm:ss.xx> inside the text are line-level polish we don't render in v1, so we
//     STRIP them (render the line whole) rather than choke on them.
//
// A file with no timestamped line at all is treated as UNSYNCED (USLT-style): synced=false, every non-empty
// raw line kept as plain text with no meaningful time — the surface shows it as a scrollable block, no
// highlight. Malformed input is best-effort and never throws.
//
// lineIndexAtTime() is the heart of the sync: the index of the last line whose time is at or before the
// playback position (−1 before the first line). The host recomputes it on each ~1 Hz position tick and pushes
// it to QML as the highlighted/auto-scrolled line.
#pragma once
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QVector>
#include <algorithm>
#include <cmath>

namespace LrcLyrics
{
    struct LyricLine
    {
        double  timeSec = 0.0; // absolute playback-second the line starts at (0 and meaningless when unsynced)
        QString text;          // the lyric text, word-level <..> tags already stripped
    };

    struct Lyrics
    {
        QVector<LyricLine> lines;
        QString            title, artist, album;
        bool               synced = false; // true iff at least one line carried a timestamp
    };

    // mm:ss(.frac) -> seconds. minutes may exceed 59; frac is centi/milli-seconds (or any digit count), scaled
    // by its own length so "5" is 0.5 and "05" is 0.05, matching how a 2-digit LRC hundredths field reads.
    inline double parseTimestamp(const QString& mm, const QString& ss, const QString& frac)
    {
        double t = mm.toInt() * 60.0 + ss.toInt();
        if (!frac.isEmpty())
            t += frac.toInt() / std::pow(10.0, frac.size());
        return t;
    }

    inline Lyrics parseLrc(const QString& text)
    {
        Lyrics out;
        // A leading [..] tag; a timestamp INSIDE such a tag; an enhanced word tag anywhere in the body text.
        static const QRegularExpression tagRe(QStringLiteral("^\\[([^\\]]*)\\]"));
        static const QRegularExpression tsRe(QStringLiteral("^(\\d+):(\\d{1,2})(?:[.:](\\d{1,3}))?$"));
        static const QRegularExpression wordRe(QStringLiteral("<\\d+:\\d{1,2}(?:[.:]\\d{1,3})?>"));

        double offsetSec = 0.0;
        QVector<LyricLine> timed;       // every timestamped (time, text) pair, pre-offset
        QStringList        unsyncedText; // non-empty plain lines, used ONLY if nothing was timestamped
        bool               anyTimed = false;

        // The repo is CRLF; split on '\n' then drop a trailing '\r' so both line endings parse identically.
        const QStringList rawLines = text.split(QChar('\n'));
        for (QString line : rawLines)
        {
            if (line.endsWith(QChar('\r')))
                line.chop(1);

            QVector<double> times; // the timestamps prefixing THIS line (usually 0 or 1, occasionally several)
            // Consume every leading bracket tag: a timestamp accumulates a time, an id tag fills metadata.
            for (;;)
            {
                const QRegularExpressionMatch m = tagRe.match(line);
                if (!m.hasMatch())
                    break;
                const QString inner = m.captured(1);
                const QRegularExpressionMatch tm = tsRe.match(inner);
                if (tm.hasMatch())
                {
                    times.push_back(parseTimestamp(tm.captured(1), tm.captured(2), tm.captured(3)));
                }
                else
                {
                    const int colon = inner.indexOf(QChar(':'));
                    if (colon > 0)
                    {
                        const QString key = inner.left(colon).trimmed().toLower();
                        const QString val = inner.mid(colon + 1).trimmed();
                        if (key == QLatin1String("ti"))          out.title  = val;
                        else if (key == QLatin1String("ar"))     out.artist = val;
                        else if (key == QLatin1String("al"))     out.album  = val;
                        else if (key == QLatin1String("offset")) offsetSec  = val.toDouble() / 1000.0;
                        // Any other id tag (by/re/ve/length) is recognised-and-ignored.
                    }
                }
                line = line.mid(m.capturedLength());
            }

            QString body = line;
            body.remove(wordRe); // strip enhanced word-level tags; render the line whole in v1
            body = body.trimmed();

            if (!times.isEmpty())
            {
                anyTimed = true;
                for (const double tt : times)
                    timed.push_back({ tt, body });
            }
            else if (!body.isEmpty())
            {
                unsyncedText << body;
            }
        }

        if (anyTimed)
        {
            out.synced = true;
            for (const LyricLine& e : timed)
            {
                // The [offset:] tag nudges every timestamp. Per the LRC convention a POSITIVE offset makes the
                // lyrics appear earlier, so we subtract it (ms -> s). A file with no offset leaves this at 0.
                out.lines.push_back({ e.timeSec - offsetSec, e.text });
            }
            std::stable_sort(out.lines.begin(), out.lines.end(),
                             [](const LyricLine& a, const LyricLine& b) { return a.timeSec < b.timeSec; });
        }
        else
        {
            out.synced = false;
            for (const QString& t : unsyncedText)
                out.lines.push_back({ 0.0, t });
        }
        return out;
    }

    // seconds -> "mm:ss.xx", the timestamp spelling parseTimestamp reads back. Minutes are NOT wrapped at 60
    // (a 74-minute live set's last line is "[74:12.30]", which is legal LRC and what parseTimestamp accepts),
    // and a negative time clamps to zero rather than rendering "[-0:03.00]", which nothing parses.
    inline QString formatTimestamp(double sec)
    {
        if (!(sec > 0.0)) // also catches NaN
            sec = 0.0;
        // Round to hundredths FIRST, then split. Truncating the seconds and rounding the fraction separately
        // turns 61.999 into "1:01.100" — a fraction that overflowed its own field.
        const qint64 total = qint64(sec * 100.0 + 0.5);
        return QStringLiteral("%1:%2.%3")
            .arg(total / 6000, 2, 10, QChar('0'))
            .arg((total / 100) % 60, 2, 10, QChar('0'))
            .arg(total % 100, 2, 10, QChar('0'));
    }

    // Lines -> an LRC document ("[mm:ss.xx]text" per line). The inverse of parseLrc for the SYNCED case, and
    // the reason it exists: ID3v2's SYLT frame is a list of (milliseconds, text) pairs, not LRC text, and a
    // second parser for it would be a second place for the sync rules to live. Rendering SYLT to LRC and
    // handing it to parseLrc keeps ONE parser — the format the rest of the app already reasons about.
    inline QString renderLrc(const QVector<LyricLine>& lines)
    {
        QString out;
        for (const LyricLine& ln : lines)
        {
            out += QLatin1Char('[');
            out += formatTimestamp(ln.timeSec);
            out += QLatin1Char(']');
            out += ln.text;
            out += QLatin1Char('\n');
        }
        return out;
    }

    // The index of the current line: the last line whose timeSec is at or before posSec, or −1 before the
    // first line begins. `lines` is sorted ascending by parseLrc, so we can stop at the first later line.
    inline int lineIndexAtTime(const Lyrics& ly, double posSec)
    {
        int idx = -1;
        for (int i = 0; i < ly.lines.size(); ++i)
        {
            if (ly.lines[i].timeSec <= posSec)
                idx = i;
            else
                break;
        }
        return idx;
    }
}
