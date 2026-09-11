// WHAT A COVER REQUEST ANSWERED (issue #370) — the one rule every cover prefetch follows, so that the copies
// of it in each client cannot drift apart.
//
// A prefetch's `then` means "new artwork landed, re-render", and a browse level wires it to a re-render that
// re-runs the prefetch over the same rows (HomeView::scheduleMusicArtRefresh). So two questions have to be
// answered after every reply, and neither may be answered by the reply having ARRIVED:
//
//   Did anything land?          Only what is on disk afterwards says so. `then` fires for that and nothing
//                               else: firing it for an answer that stored nothing re-renders the level, which
//                               finds nothing on disk and nothing in flight, and asks again — for ever. That
//                               loop is #370, and before it the Jellyfin and server-shelf clients fired `then`
//                               even for a cover that was ALREADY cached.
//
//   Should it be asked again?   That depends on who said no.
//
//     Image    a body whose OWN BYTES say it is a picture (isPicture, below). Stored; `then` fires if, and
//              only if, it is then on disk.
//     Absent   the SERVER answered, and the answer was "no picture": a successful empty body, a 404 or a 410
//              (or, for Subsonic, the protocol's own "not found" — see Subsonic::coverAnswer). Remembered as
//              "no art" for the SESSION, in memory only and keyed on the album key: never persisted, so art the
//              server gains later still arrives in a later session; and never a url, which can carry a
//              credential.
//     Retry    nothing was answered: a refused connection, a TIMEOUT, a 5xx, a refused credential — or a 200
//              whose body is NOT a picture (#377): a reverse proxy's error page, a captive portal, a login
//              page. That is something in the way talking, not the server saying "no art". Stored, it would be
//              a broken picture that is also "already on disk", so the real art would never be asked for again.
//              Not remembered, because it may work next time — and not a reason to re-render either, so it is
//              asked again only when the level re-renders for some other reason.
#pragma once
#include <QByteArray>

#include <cstring>

namespace CoverFetch
{
    enum class Answer { Image, Absent, Retry };

    namespace detail
    {
        inline bool bytesAt(const QByteArray& b, qsizetype at, const char* sig, qsizetype n)
        {
            return b.size() >= at + n && std::memcmp(b.constData() + at, sig, size_t(n)) == 0;
        }
        inline bool xmlSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

        // An SVG is text, so it has no magic number; what it has is a ROOT ELEMENT named svg. Skipped on the
        // way to it: a UTF-8 BOM, whitespace, the XML declaration, comments and a DOCTYPE (internal subset and
        // all). The root and not "contains <svg": an HTML page with an icon drawn in it is a page.
        inline bool svgDocument(const QByteArray& b)
        {
            const qsizetype n = b.size();
            qsizetype i = bytesAt(b, 0, "\xEF\xBB\xBF", 3) ? 3 : 0;
            for (;;)
            {
                while (i < n && xmlSpace(b[i])) ++i;
                qsizetype end = -1;
                if (bytesAt(b, i, "<?", 2))
                {
                    if ((end = b.indexOf("?>", i + 2)) < 0) return false;
                    i = end + 2;
                }
                else if (bytesAt(b, i, "<!--", 4))
                {
                    if ((end = b.indexOf("-->", i + 4)) < 0) return false;
                    i = end + 3;
                }
                else if (bytesAt(b, i, "<!DOCTYPE", 9))
                {
                    const qsizetype subset = b.indexOf('[', i);
                    const qsizetype close  = b.indexOf('>', i);
                    if (close < 0) return false;
                    if (subset >= 0 && subset < close)
                    {
                        const qsizetype subsetEnd = b.indexOf(']', subset);
                        if (subsetEnd < 0 || (end = b.indexOf('>', subsetEnd)) < 0) return false;
                        i = end + 1;
                    }
                    else i = close + 1;
                }
                else break;
            }
            if (!bytesAt(b, i, "<svg", 4) || i + 4 >= n) return false;
            const char after = b[i + 4];
            return xmlSpace(after) || after == '>' || after == '/';
        }
    }

    // IS THIS BODY A PICTURE? (#377) Asked of the BYTES, never of the Content-Type header: the header is exactly
    // the part a misbehaving proxy gets wrong — a 200 labelled image/jpeg wrapped round an HTML page. The set is
    // the formats MetaCache can hold (MetaCache.cpp, imageExts(): jpg/jpeg, png, webp, gif, svg), each by its
    // own signature:
    //   JPEG  FF D8 FF                          PNG   89 50 4E 47 0D 0A 1A 0A
    //   GIF   "GIF87a" or "GIF89a"              WebP  "RIFF", a four-byte size, "WEBP"
    //   SVG   text whose root element is <svg>  (see detail::svgDocument)
    // Anything else is not a cover. Pure: no I/O, no state.
    inline bool isPicture(const QByteArray& b)
    {
        return detail::bytesAt(b, 0, "\xFF\xD8\xFF", 3)
            || detail::bytesAt(b, 0, "\x89PNG\r\n\x1A\n", 8)
            || detail::bytesAt(b, 0, "GIF87a", 6) || detail::bytesAt(b, 0, "GIF89a", 6)
            || (detail::bytesAt(b, 0, "RIFF", 4) && detail::bytesAt(b, 8, "WEBP", 4))
            || detail::svgDocument(b);
    }

    // `transportOk` is QNetworkReply::NoError. `httpStatus` is the status line's code, and 0 when there was
    // none — a refused connection, and a timeout, both look like that. The body is only read when the transfer
    // succeeded: a failed reply's body is an error page, not a picture. Neither is the header consulted.
    inline Answer classify(bool transportOk, int httpStatus, const QByteArray& body)
    {
        if (!transportOk)
            return (httpStatus == 404 || httpStatus == 410) ? Answer::Absent : Answer::Retry;
        if (body.isEmpty()) return Answer::Absent;
        return isPicture(body) ? Answer::Image : Answer::Retry;
    }

    // THE TIMEOUT RULE. A cover request that goes this long without a byte moving is given up (it is
    // QNetworkRequest's transfer timeout, an INACTIVITY bound rather than a total), and given up is Retry, never
    // Absent: a slow server has not said "no art". It is bounded at all because without it a hung request
    // holds its in-flight tag — and with it every later ask for that cover — for the rest of the session.
    constexpr int kTransferTimeoutMs = 20000;
}
