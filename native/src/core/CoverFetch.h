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
        inline quint32 le16(const QByteArray& b, qsizetype at) { return quint8(b[at]) | quint32(quint8(b[at + 1])) << 8; }
        inline quint32 le32(const QByteArray& b, qsizetype at) { return le16(b, at) | le16(b, at + 2) << 16; }
        inline quint32 be32(const QByteArray& b, qsizetype at)
        {
            return quint32(quint8(b[at])) << 24 | quint32(quint8(b[at + 1])) << 16 | quint32(quint8(b[at + 2])) << 8
                 | quint8(b[at + 3]);
        }

        // BMP: "BM", and a DIB header size Qt's own reader accepts at bytes 14-17 (OS/2 1.x 12, OS/2 2.x 16 or 64,
        // BITMAPINFOHEADER 40, its v2/v3 extensions 52/56, v4 108, v5 124). "BM" alone would wave through any text
        // that starts with those two letters; the size field holds NUL bytes, which no text does.
        inline bool bmp(const QByteArray& b)
        {
            if (!bytesAt(b, 0, "BM", 2) || b.size() < 18) return false;
            switch (le32(b, 14))
            {
            case 12: case 16: case 40: case 52: case 56: case 64: case 108: case 124: return true;
            default: return false;
            }
        }
        // ICO 00 00 01 00 / CUR 00 00 02 00, then the image count: a header that declares NO images is not a
        // picture, and neither is a four-byte prefix with nothing after it.
        inline bool icon(const QByteArray& b)
        {
            return (bytesAt(b, 0, "\x00\x00\x01\x00", 4) || bytesAt(b, 0, "\x00\x00\x02\x00", 4))
                && b.size() >= 6 && le16(b, 4) != 0;
        }
        // TIFF "II*\0" (little-endian) / "MM\0*" (big-endian), then the first IFD's offset, which cannot point back
        // inside the 8-byte header.
        inline bool tiff(const QByteArray& b)
        {
            if (b.size() < 8) return false;
            if (bytesAt(b, 0, "II*\x00", 4)) return le32(b, 4) >= 8;
            if (bytesAt(b, 0, "MM\x00*", 4)) return be32(b, 4) >= 8;
            return false;
        }

        // Where the ROOT ELEMENT of a markup document can start: past an optional UTF-8 BOM, whitespace, the XML
        // declaration, comments and a DOCTYPE (internal subset and all). -1 when one of those does not close
        // inside `b` - which, for bytes read back from a file's PREFIX (#382), means "ran out", not "not SVG".
        inline qsizetype prologEnd(const QByteArray& b)
        {
            const qsizetype n = b.size();
            qsizetype i = bytesAt(b, 0, "\xEF\xBB\xBF", 3) ? 3 : 0;
            for (;;)
            {
                while (i < n && xmlSpace(b[i])) ++i;
                qsizetype end = -1;
                if (bytesAt(b, i, "<?", 2))
                {
                    if ((end = b.indexOf("?>", i + 2)) < 0) return -1;
                    i = end + 2;
                }
                else if (bytesAt(b, i, "<!--", 4))
                {
                    if ((end = b.indexOf("-->", i + 4)) < 0) return -1;
                    i = end + 3;
                }
                else if (bytesAt(b, i, "<!DOCTYPE", 9))
                {
                    const qsizetype subset = b.indexOf('[', i);
                    const qsizetype close  = b.indexOf('>', i);
                    if (close < 0) return -1;
                    if (subset >= 0 && subset < close)
                    {
                        const qsizetype subsetEnd = b.indexOf(']', subset);
                        if (subsetEnd < 0 || (end = b.indexOf('>', subsetEnd)) < 0) return -1;
                        i = end + 1;
                    }
                    else i = close + 1;
                }
                else return i;
            }
        }

        // An SVG is text, so it has no magic number; what it has is a ROOT ELEMENT named svg, found past the
        // prolog (prologEnd). The root and not "contains <svg": an HTML page with an icon drawn in it is a page.
        inline bool svgDocument(const QByteArray& b)
        {
            const qsizetype n = b.size();
            const qsizetype i = prologEnd(b);
            if (i < 0 || !bytesAt(b, i, "<svg", 4) || i + 4 >= n) return false;
            const char after = b[i + 4];
            return xmlSpace(after) || after == '>' || after == '/';
        }
    }

    // IS THIS BODY A PICTURE? (#377) Asked of the BYTES, never of the Content-Type header: the header is exactly
    // the part a misbehaving proxy gets wrong — a 200 labelled image/jpeg wrapped round an HTML page. The set is
    // the formats MetaCache can hold (MetaCache.cpp, imageExts()), each by its own signature:
    //   JPEG  FF D8 FF                          PNG   89 50 4E 47 0D 0A 1A 0A
    //   GIF   "GIF87a" or "GIF89a"              WebP  "RIFF", a four-byte size, "WEBP"
    //   SVG   text whose root element is <svg>  (see detail::svgDocument)
    //   BMP   "BM" + a known DIB header size    ICO / CUR  00 00 01 00 / 00 00 02 00 + a non-zero image count
    //   TIFF  "II*\0" or "MM\0*" + a first-IFD offset past the header
    // BMP, ICO/CUR and TIFF joined with #387: the classic grid stores any thumb Qt decodes, and a picture this rule
    // did not know was removed by the read-back and fetched again on every visit. No TEXT body can pass any of them -
    // each needs a NUL byte where text has none - so an error page, an XML error or a login page never does.
    // Anything else is not a cover. Pure: no I/O, no state.
    inline bool isPicture(const QByteArray& b)
    {
        return detail::bytesAt(b, 0, "\xFF\xD8\xFF", 3)
            || detail::bytesAt(b, 0, "\x89PNG\r\n\x1A\n", 8)
            || detail::bytesAt(b, 0, "GIF87a", 6) || detail::bytesAt(b, 0, "GIF89a", 6)
            || (detail::bytesAt(b, 0, "RIFF", 4) && detail::bytesAt(b, 8, "WEBP", 4))
            || detail::bmp(b) || detail::icon(b) || detail::tiff(b)
            || detail::svgDocument(b);
    }

    // WHAT THE CLASSIC GRID MAY CACHE (#389). The grid's thumbnail loader (HomeView::pumpThumbnails) decodes every
    // remote thumb it fetches, and used to store whatever decoded. Qt decodes more than the cache holds: PBM/PGM/PPM,
    // XBM and XPM are TEXT, and TGA has no signature at all, so isPicture refuses them - rightly, or an error page
    // could pass for art again (#377). Stored anyway, such a thumb was judged broken by the next read-back
    // (verifiedImagePath), removed, and fetched and stored again on every visit. So the grid stores a thumb only
    // when it decoded AND the cache's own rule says it will keep it: one rule for what the cache holds. A decoded
    // thumb that fails this is still painted - from the bytes the grid already has - just never written. Pure.
    inline bool gridThumbCacheable(bool decoded, const QByteArray& body)
    {
        return decoded && isPicture(body);
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

    // A COVER ALREADY ON DISK, READ BACK (#382). Covers stored before #377 may be an error page saved as cover.jpg,
    // and "is it already on disk?" says yes to that for ever. So a stored cover is judged by the same bytes rule as
    // a reply - but from a bounded PREFIX of the file, since the whole file must never be read on the GUI thread.
    //
    //   kSignatureBytes     every raster signature above ends by byte 18 (BMP's DIB header size is bytes 14-17; WebP's
    //                       "WEBP" is 8-11; ICO/CUR's count 4-5; TIFF's IFD offset 4-7), so a JPEG/PNG/GIF/WebP/BMP/
    //                       ICO/CUR/TIFF picture is decided from its first 18 bytes and nothing more is read.
    //   kStoredPrefixBytes  64 KiB, read only when those 12 bytes are not a raster picture - which is SVG, or
    //                       something broken. An SVG's root element follows its prolog, and the prologs real
    //                       exporters write (an XML declaration, a generator comment, Illustrator's DOCTYPE with
    //                       its entity subset) run to a few KB; 64 KiB is many times that. A broken page decides
    //                       far sooner: "<!DOCTYPE html>" closes at byte 15 and the root after it is <html>.
    //
    // THE RULE ERRS TOWARD KEEPING. A valid cover wrongly judged broken is deleted and fetched again, which is the
    // regression that would matter; a broken one wrongly kept is today's behaviour. So when the prefix is SHORTER
    // than the file and it ran out before a root element could be seen (a prolog longer than 64 KiB, or a root that
    // starts at the very end of it), the answer is "intact". Only a decided "not a picture" is broken. Pure.
    constexpr qsizetype kSignatureBytes    = 18;
    constexpr qsizetype kStoredPrefixBytes = 64 * 1024;
    inline bool storedCoverIntact(const QByteArray& prefix, qint64 fileSize)
    {
        if (isPicture(prefix)) return true;
        if (fileSize <= prefix.size()) return false;           // the whole file was read: decided
        const qsizetype root = detail::prologEnd(prefix);
        return root < 0 || root + 4 >= prefix.size();          // ran out before the root: undecided, so kept
    }

    // THE TIMEOUT RULE. A cover request that goes this long without a byte moving is given up (it is
    // QNetworkRequest's transfer timeout, an INACTIVITY bound rather than a total), and given up is Retry, never
    // Absent: a slow server has not said "no art". It is bounded at all because without it a hung request
    // holds its in-flight tag — and with it every later ask for that cover — for the rest of the session.
    constexpr int kTransferTimeoutMs = 20000;
}
