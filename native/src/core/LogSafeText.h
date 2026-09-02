// A URL, AND FREE TEXT THAT MIGHT CONTAIN ONE, AS THEY MAY BE **LOGGED** (issue #231).
// Pure, header-only, QtCore-only.
//
// StoredUrl is the rule for a url that is STORED. This is the rule for one that is WRITTEN INTO
// stream_debug.log, and the difference between the two is one sentence: a stored location has to stay
// re-openable, a logged one does not. So this composes StoredUrl::location() — the canonical #200 rule,
// userinfo and query and fragment gone, anything that is not a network url returned byte for byte — and then
// takes the one further step location() explicitly declines to take, collapsing the path's MIDDLE segments.
// StoredUrl's header says why it leaves a path alone ("no heuristic can tell a token segment from a content
// id, and a wrong guess mangles every legitimate stream url in the store"); every word of that is about a
// store. A log line loses nothing by naming a file rather than the seven segments above it, and the addon
// access tokens that ride in a path segment are exactly the thing this file exists to keep out of a bug
// report.
//
// WHY IT IS A HEADER AND NOT A FOURTH COPY. `scheme://host[:port]/…/<filename>` was already the project's
// log rule; it was simply written out three times, as a file-static, in MainWindow.cpp, DownloadManager.cpp
// and StreamResolver.cpp. #231 needs a fourth caller, and a fourth copy of a redaction rule is how one of
// them ends up a revision behind the other three. The three statics now delegate here and keep their names,
// so their call sites are untouched and the rendering is byte-for-byte what it was.
//
// ---- scrub(): THE PART THAT IS NEW ---------------------------------------------------------------------
//
// Everything above takes a string that IS a url. mpv's log messages are prose with a url somewhere in them:
//
//   [ffmpeg/demuxer] http: Will reconnect at 1048576 in 0 second(s), error=Connection reset by peer
//   [stream] Opening https://cdn.example.net/dl/9f2c/Movie.mkv?token=<32 opaque chars>&exp=1756742400
//   [file] Cannot open file 'https://user:pw@host/x': Permission denied
//
// so before the rule can be applied, the url has to be FOUND. scrub() does only that: it locates each
// "<scheme>://…" run in the text, hands it to url(), and leaves every other character alone. It is
// deliberately not a credential detector — it makes no judgement about what a token looks like, which is the
// property that lets it survive the next provider spelling its parameter something new.
//
// The run ends at the first character a url cannot contain (whitespace, a quote, a bracket) and then any
// trailing sentence punctuation is handed back to the prose, so "…mkv?token=abc." keeps its full stop and
// "'https://…'" keeps its quotes. A url is never lengthened, so an offset walk over the original is safe.
#pragma once
#include <QChar>
#include <QFileInfo>
#include <QLatin1String>
#include <QString>
#include <QUrl>
#include "StoredUrl.h"

namespace LogSafeText
{
// A log-safe rendering of a URL: scheme://host[:port]/…/<filename>. Drops userinfo, the query and the
// fragment (StoredUrl::location's rule — debrid keys ride there) and the path's middle segments (which can
// carry an addon access token). A local path is reduced to its file name, as it always was.
inline QString url(const QString& s)
{
    const QString loc = StoredUrl::location(s);   // the #200 rule first, so there is one place it is stated
    const QUrl u(loc);
    if (u.scheme().isEmpty()) return QFileInfo(loc).fileName(); // a local path
    const QString file = QFileInfo(u.path()).fileName();
    return u.scheme() + QStringLiteral("://") + u.host()
         + (u.port() > 0 ? QStringLiteral(":") + QString::number(u.port()) : QString())
         + QStringLiteral("/…/") + file;
}

namespace detail
{
// Whether `c` may appear inside a url as written in prose. Everything RFC 3986 allows, minus the characters
// that in practice end one in a sentence — whitespace, the quote marks a message wraps a path in, and the
// angle brackets a few libraries use. '(' and ')' are legal in a url and are kept; the trailing-punctuation
// trim below is what deals with the "(see https://x)" case.
inline bool urlChar(QChar c)
{
    if (c.isSpace()) return false;
    const ushort u = c.unicode();
    if (u > 126) return false;                                    // non-ASCII ends the run
    return !(u == '"' || u == '\'' || u == '`' || u == '<' || u == '>' || u == '|' || u == '^' || u == '{'
             || u == '}' || u == '\\');
}

// Characters that end a SENTENCE rather than a url, when they are the last thing in the run. Trimmed one at
// a time from the right so "…/a.mkv?x=1)." gives back both.
inline bool trailingPunct(QChar c)
{
    const ushort u = c.unicode();
    return u == '.' || u == ',' || u == ';' || u == ':' || u == ')' || u == ']' || u == '!' || u == '?';
}

// Start of the scheme that ends at the "://" beginning at `sep`, or -1 when the characters before it are not
// a scheme. RFC 3986: ALPHA *( ALPHA / DIGIT / "+" / "-" / "." ), and it must start with a letter.
inline int schemeStart(const QString& s, int sep)
{
    int i = sep;
    while (i > 0)
    {
        const QChar c = s.at(i - 1);
        if (c.isLetterOrNumber() && c.unicode() < 128) { --i; continue; }
        if (c == QLatin1Char('+') || c == QLatin1Char('-') || c == QLatin1Char('.')) { --i; continue; }
        break;
    }
    if (i == sep) return -1;                       // nothing before "://"
    const QChar first = s.at(i);
    if (!(first.isLetter() && first.unicode() < 128)) return -1;
    return i;
}
} // namespace detail

// FREE TEXT with every url in it rendered through url(). Anything that is not a url is returned byte for
// byte, including a message with no url in it at all (the overwhelmingly common case, and the one that must
// cost nothing and change nothing).
inline QString scrub(const QString& text)
{
    if (!text.contains(QLatin1String("://"))) return text;   // the fast, and usual, way out

    QString out;
    out.reserve(text.size());
    int copied = 0;
    for (int sep = text.indexOf(QLatin1String("://")); sep >= 0;
         sep = text.indexOf(QLatin1String("://"), sep + 3))
    {
        const int start = detail::schemeStart(text, sep);
        if (start < 0 || start < copied) continue;           // not a scheme, or inside a url already handled

        int end = sep + 3;
        while (end < text.size() && detail::urlChar(text.at(end))) ++end;
        while (end > sep + 3 && detail::trailingPunct(text.at(end - 1))) --end;

        out += text.mid(copied, start - copied);
        out += url(text.mid(start, end - start));
        copied = end;
    }
    out += text.mid(copied);
    return out;
}
} // namespace LogSafeText
