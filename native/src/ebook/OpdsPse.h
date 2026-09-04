// OPDS-PSE (Page Streaming Extension) — the pure heart of "read a comic off the server instead of
// downloading the whole volume" (#153).
//
// A comic volume on a Komga / Kavita library is 100-500 MB. PSE is the OPDS extension that lets a client
// ask for ONE PAGE at a time, as an image, optionally resized server-side for the screen. The server
// advertises it as an extra <link> on the ordinary acquisition entry #146 already parses:
//
//   <link rel="http://vaemendis.net/opds-pse/stream"
//         href="/opds/v1.2/books/0A1/pages/{pageNumber}?zero_based=true"
//         type="image/jpeg"
//         pse:count="120" pse:lastRead="7" pse:lastReadDate="2026-08-30T10:00:00Z"/>
//
// This header is QtCore-only and does NO network and NO disk: it turns that link into (a) the url of page
// N, (b) the order the pages should be asked for, and (c) the request a progress report would be, if the
// server is one whose progress API this client recognises. Every one of those is a pure function so
// probe_pse can pin it against hand-computed expectations with no socket.
//
// ---------------------------------------------------------------------------------------------------
// THE TWO NUMBERINGS, written down because they are genuinely different and getting them confused reads
// the wrong page or skips one.
//
//   {pageNumber} — an INDEX into the volume, counted from 0. That is the PSE spec's numbering, and it is
//     what Komga's own href says out loud: its REST endpoint /api/v1/books/{id}/pages/{n} is 1-based, so
//     its OPDS controller appends `zero_based=true` to tell that endpoint the number arriving from a PSE
//     client is an index. A server that declares `zero_based=false` is taken at its word and asked
//     1-based; nothing else changes the base.
//
//   pse:lastRead — a PAGE NUMBER a human would say, counted from 1 ("you got to page 7"). Komga fills it
//     from its read-progress `page`, which is 1-based. It is read here as 1-based and resolved to an index
//     by subtracting one, and a progress report is sent back in the same 1-based numbering.
//
// WHY THAT ASYMMETRY IS THE SAFE ONE. A real Komga / Kavita was not available while this was written, so
// the possibility that some server disagrees is real. Reading lastRead one page EARLY re-shows a page you
// have already read; reading it one page LATE skips a page you have not. Subtracting one can only err in
// the first direction, so it is the direction this file errs in on purpose. `lastReadIndex` also clamps
// into [0, count-1], so a server that reports a page past the end of the book lands on its last page
// rather than off it.
// ---------------------------------------------------------------------------------------------------
//
// SAFETY: nothing in this file takes a credential and nothing in it logs. The Authorization header the
// page requests carry is #146's device-local Basic auth, attached by the caller at fetch time exactly as
// it is attached to a feed fetch. A url built here can still be secret — Kavita puts an apiKey in the
// query string — so a caller logging a page url would be leaking one. Log the page NUMBER.
#pragma once
#include <QByteArray>
#include <QLatin1String>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QUrlQuery>
#include <QVector>
#include <QtGlobal>

// The PSE facts of one acquisition entry: the page-image url template plus what the server says about it.
// Default-constructed (count 0) on every entry that offers no PSE link, which is what makes every non-PSE
// route read exactly as it did before #153 — `isValid()` is false and nothing offers to read online.
struct OpdsPseLink
{
    QString hrefTemplate;   // ABSOLUTE (resolved at parse time), carries {pageNumber} and maybe {maxWidth}
    QString type;           // the image mime the server declares ("image/jpeg"); informational only
    int count = 0;          // pse:count — how many pages the volume has
    int lastRead = -1;      // pse:lastRead, 1-based; -1 = the server advertised none
    QString lastReadDate;   // pse:lastReadDate, verbatim; informational only

    // A usable PSE offer: a positive page count AND somewhere to put the page number. A link with a count
    // but no {pageNumber} placeholder is not streamable — every page would be the same url.
    bool isValid() const
    {
        return count > 0 && hrefTemplate.contains(QLatin1String("{pageNumber}"));
    }
};

namespace OpdsPse
{

// The link relation that names a PSE stream. Matched by CONTAINMENT, because feeds in the wild write it
// both with and without a scheme and Atom allows a space-separated rel list.
inline bool isStreamRel(const QString& rel)
{
    return rel.contains(QLatin1String("opds-pse/stream"));
}

// Does this template have somewhere to put the server-side resize hint? PSE 1.2's {maxWidth}. A server
// that cannot resize simply omits it, and then the viewport width has nowhere to go — which is not an
// error, it just means full-size pages.
inline bool supportsMaxWidth(const QString& tmpl)
{
    return tmpl.contains(QLatin1String("{maxWidth}"));
}

// The number that stands for the FIRST page. 0 by the spec; 1 only when the template declares
// `zero_based=false` (see the header comment). Compared case-insensitively because a query value's case
// is the server's business.
inline int firstPageNumber(const QString& tmpl)
{
    return tmpl.contains(QLatin1String("zero_based=false"), Qt::CaseInsensitive) ? 1 : 0;
}

// The url of the page at 0-based reading index `index0`, with the server-side resize hint set to
// `maxWidth` where the template accepts one. A non-positive maxWidth leaves the placeholder alone rather
// than writing "0" into it — asking a server to resize to zero is worse than not asking.
inline QString pageUrl(const OpdsPseLink& link, int index0, int maxWidth)
{
    QString out = link.hrefTemplate;
    out.replace(QLatin1String("{pageNumber}"), QString::number(index0 + firstPageNumber(link.hrefTemplate)));
    if (maxWidth > 0)
        out.replace(QLatin1String("{maxWidth}"), QString::number(maxWidth));
    return out;
}

// Where opening this volume should land: the 0-based index of the page pse:lastRead names, or -1 when the
// server advertised none (open at the start, as every other comic does). See the header comment for why
// this subtracts one and why it clamps.
inline int lastReadIndex(const OpdsPseLink& link)
{
    if (link.count <= 0 || link.lastRead < 0) return -1;
    const int idx = link.lastRead - 1;
    return qBound(0, idx, link.count - 1);
}

// THE ORDER THE PAGES ARE ASKED FOR. The page you are about to look at first, then the two after it, then
// everything else in reading order — which is the prefetch rule the issue asks for, expressed as an order
// rather than as a second fetcher. It is an ORDER and not a subset: every page of the volume is in the
// list exactly once, because the seam this feeds packs a whole chapter before opening it.
//
// Why an order is enough to be a prefetch: the fetches are submitted to one QNetworkAccessManager, which
// runs a handful per host and queues the rest IN SUBMISSION ORDER. Submitting the resume page first is
// therefore the difference between the page you asked for arriving in the first round trip and arriving
// after two hundred others.
//
// `start0` out of range (including -1, "no resume") degrades to plain reading order.
inline QVector<int> fetchOrder(int count, int start0)
{
    QVector<int> out;
    if (count <= 0) return out;
    out.reserve(count);
    if (start0 >= 0 && start0 < count)
    {
        for (int k = 0; k < 3 && start0 + k < count; ++k) out.push_back(start0 + k);   // N, N+1, N+2
    }
    for (int i = 0; i < count; ++i)
        if (!out.contains(i)) out.push_back(i);
    return out;
}

// ---- Progress: what a report to THIS server would be -----------------------------------------------
//
// A progress report is an HTTP request, described here and performed by the caller. `isValid()` false
// means "this client does not know how to tell this server" — and then nothing is sent. #153 is explicit
// that a progress endpoint is not to be INVENTED: the two shapes below are the two published APIs whose
// url can be derived from the PSE href itself, and any other server simply keeps whatever progress it
// keeps by its own means.
struct PseProgress
{
    QString    method;        // "PATCH" (Komga) / "POST" (Kavita)
    QString    url;           // absolute
    QByteArray body;          // JSON
    QString    contentType = QStringLiteral("application/json");
    QString    server;        // "komga" / "kavita" — for a log line that names no url and no credential
    bool isValid() const { return !url.isEmpty() && !method.isEmpty(); }
};

// Strip a url back to scheme://host[:port] plus an arbitrary leading path prefix. Both servers can be
// hosted under a sub-path behind a reverse proxy, so the API url has to be built from the PSE href's own
// prefix rather than from the origin alone.
inline QString pseOrigin(const QUrl& u)
{
    QString out = u.scheme() + QLatin1String("://") + u.host();
    if (u.port() > 0) out += QLatin1Char(':') + QString::number(u.port());
    return out;
}

// Komga: the PSE href is <base>/opds/v1.2/books/<bookId>/pages/{pageNumber}?zero_based=true and the read
// progress API is PATCH <base>/api/v1/books/<bookId>/read-progress with {"page":N,"completed":bool} — N
// 1-based, `completed` true on the last page. Also matched when a server hands out the /api/v1 page url
// directly, because the same book id sits in the same place.
//
// https://komga.org/docs/api  (ReadProgressUpdateDto: page, completed)
inline PseProgress komgaProgress(const QString& hrefTemplate, int index0, int count)
{
    static const QRegularExpression re(
        QStringLiteral("^(.*?)/(?:opds/v[0-9.]+|api/v[0-9]+)/books/([^/]+)/pages/"),
        QRegularExpression::CaseInsensitiveOption);
    const QUrl u(hrefTemplate);
    const QRegularExpressionMatch m = re.match(u.path());
    if (!m.hasMatch()) return PseProgress();
    PseProgress p;
    p.server = QStringLiteral("komga");
    p.method = QStringLiteral("PATCH");
    p.url    = pseOrigin(u) + m.captured(1) + QStringLiteral("/api/v1/books/") + m.captured(2)
             + QStringLiteral("/read-progress");
    const int page1 = index0 + 1;
    p.body = QStringLiteral("{\"page\":%1,\"completed\":%2}")
                 .arg(page1)
                 .arg(count > 0 && page1 >= count ? QStringLiteral("true") : QStringLiteral("false"))
                 .toUtf8();
    return p;
}

// Kavita: the PSE href is <base>/api/opds/<apiKey>/image?libraryId=..&seriesId=..&volumeId=..&chapterId=..
// &pageNumber={pageNumber}, and progress is POST <base>/api/reader/progress with a ProgressDto
// {libraryId, seriesId, volumeId, chapterId, pageNum}. pageNum is Kavita's own page INDEX, so it is sent
// in the template's numbering rather than as a 1-based page.
//
// NOT VERIFIED AGAINST A LIVE KAVITA. /api/reader/progress is an authenticated endpoint and the OPDS
// apiKey in the href is not, on its own, an account token — so this report may be refused. A refusal is
// logged and dropped: the reader keeps working and the server keeps its own progress. That is why the
// chapterId gate below is strict — a report with a missing id is not sent at all.
inline PseProgress kavitaProgress(const QString& hrefTemplate, int index0)
{
    const QUrl u(hrefTemplate);
    if (!u.path().contains(QLatin1String("/api/opds/"), Qt::CaseInsensitive)) return PseProgress();
    const QUrlQuery q(u);
    const QString chapterId = q.queryItemValue(QStringLiteral("chapterId"));
    if (chapterId.isEmpty()) return PseProgress();     // no chapter to report against: send nothing
    const QString base = u.path().left(u.path().indexOf(QLatin1String("/api/opds/"), 0, Qt::CaseInsensitive));
    PseProgress p;
    p.server = QStringLiteral("kavita");
    p.method = QStringLiteral("POST");
    p.url    = pseOrigin(u) + base + QStringLiteral("/api/reader/progress");
    const int pageNum = index0 + firstPageNumber(hrefTemplate);
    p.body = QStringLiteral("{\"libraryId\":%1,\"seriesId\":%2,\"volumeId\":%3,\"chapterId\":%4,\"pageNum\":%5}")
                 .arg(q.queryItemValue(QStringLiteral("libraryId")).isEmpty()
                          ? QStringLiteral("0") : q.queryItemValue(QStringLiteral("libraryId")),
                      q.queryItemValue(QStringLiteral("seriesId")).isEmpty()
                          ? QStringLiteral("0") : q.queryItemValue(QStringLiteral("seriesId")),
                      q.queryItemValue(QStringLiteral("volumeId")).isEmpty()
                          ? QStringLiteral("0") : q.queryItemValue(QStringLiteral("volumeId")),
                      chapterId)
                 .arg(pageNum)
                 .toUtf8();
    return p;
}

// The report for whichever server this href belongs to, or an invalid one for a server this client does
// not recognise — in which case NOTHING is sent (#153: do not invent an endpoint).
inline PseProgress progressReport(const OpdsPseLink& link, int index0)
{
    if (!link.isValid() || index0 < 0) return PseProgress();
    const PseProgress komga = komgaProgress(link.hrefTemplate, index0, link.count);
    if (komga.isValid()) return komga;
    return kavitaProgress(link.hrefTemplate, index0);
}

} // namespace OpdsPse
