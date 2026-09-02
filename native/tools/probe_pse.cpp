// Headless check of OPDS-PSE — reading a comic volume a page at a time off the server that holds it
// (#153). Everything under test here is PURE (src/ebook/OpdsPse.h + OpdsPsePages.h): a url template and a
// page count in, a page url / an asking order / an HTTP request description out. No socket, no disk.
//
//   1. RECOGNITION — parseOpds lifts a pse:stream link off an acquisition entry: the template, pse:count,
//      pse:lastRead, pse:lastReadDate. An entry with no pse:lastRead is NOT "you are on page 0". A
//      relative template survives href resolution with its {pageNumber} placeholder intact.
//   2. EXPANSION — page N's url, with and without a {maxWidth} the template accepts, and the zero_based
//      declaration that decides whether page one is asked for as 0 or as 1.
//   3. RESUME — pse:lastRead resolves to a 0-based index, one page EARLY by design, clamped into the
//      book, and -1 ("say nothing") when the server advertised none.
//   4. PREFETCH ORDER — the resume page, then the two after it, then the rest, every page exactly once.
//   5. THE PAGE LIST — one entry per page, in reading order, each carrying the catalog's credentials.
//   6. PROGRESS — the Komga and Kavita request shapes, and an invalid report (send NOTHING) for a server
//      whose progress API this client does not know. #153: do not invent an endpoint.
//   7. CREDENTIALS — a byte scan. The fixture password appears in NO page url, in NO progress url and in
//      NO progress body: it lives in a request header and nowhere else.
//
// FIXTURE INDEPENDENCE: every expected string below is written out by hand. Nothing here is computed by
// running the code under test, so the rules are measured against an oracle that does not run them.
//
// Prints PSE-OK on success; any failure prints PSE-FAIL <cond> (line) and exits non-zero.
#include "OpdsFeed.h"
#include "OpdsPse.h"
#include "OpdsPsePages.h"
#include "PageSupply.h"

#include <QCoreApplication>
#include <algorithm>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "PSE-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

// A Komga-shaped acquisition feed: one entry offering BOTH a downloadable CBZ and a PSE page stream (the
// "Read online beside Download" case), and one entry offering a PSE stream with NO pse:lastRead (a volume
// the server has no progress for). The hrefs are ROOT-RELATIVE, as Komga's really are, so this also
// exercises href resolution with a {pageNumber} placeholder in it.
static const char* kFeed = R"(<?xml version="1.0" encoding="utf-8"?>
<feed xmlns="http://www.w3.org/2005/Atom"
      xmlns:opds="http://opds-spec.org/2010/catalog"
      xmlns:pse="http://vaemendis.net/opds-pse/ns">
  <id>urn:series:42</id>
  <title>Saga</title>
  <entry>
    <title>Saga, Vol. 1</title>
    <id>urn:book:0A1</id>
    <link rel="http://opds-spec.org/image" href="/covers/0A1.png" type="image/png"/>
    <link rel="http://opds-spec.org/acquisition" href="/opds/v1.2/books/0A1/file/saga-1.cbz"
          type="application/vnd.comicbook+zip"/>
    <link rel="http://vaemendis.net/opds-pse/stream" href="/opds/v1.2/books/0A1/pages/{pageNumber}?zero_based=true"
          type="image/jpeg" pse:count="160" pse:lastRead="7" pse:lastReadDate="2026-08-30T10:00:00Z"/>
  </entry>
  <entry>
    <title>Saga, Vol. 2</title>
    <id>urn:book:0A2</id>
    <link rel="http://opds-spec.org/acquisition" href="/opds/v1.2/books/0A2/file/saga-2.cbz"
          type="application/vnd.comicbook+zip"/>
    <link rel="http://vaemendis.net/opds-pse/stream" href="/opds/v1.2/books/0A2/pages/{pageNumber}?zero_based=true"
          type="image/jpeg" pse:count="152"/>
  </entry>
  <entry>
    <title>A Plain Book</title>
    <id>urn:book:0B0</id>
    <link rel="http://opds-spec.org/acquisition" href="/opds/v1.2/books/0B0/file/plain.epub"
          type="application/epub+zip"/>
  </entry>
</feed>)";

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // ================= 1. Recognition ==================================================================
    const OpdsFeed feed = parseOpds(QByteArray(kFeed), QStringLiteral("http://komga.lan/opds/v1.2/series/42"));
    CHECK(feed.entries.size() == 3);
    if (feed.entries.size() == 3)
    {
        const OpdsEntry& v1 = feed.entries[0];
        const OpdsEntry& v2 = feed.entries[1];
        const OpdsEntry& plain = feed.entries[2];

        // The PSE link is lifted, and it is NOT mistaken for an acquisition: the download is still the CBZ.
        CHECK(v1.pse.isValid());
        CHECK(v1.pse.count == 160);
        CHECK(v1.pse.lastRead == 7);
        CHECK(v1.pse.lastReadDate == QStringLiteral("2026-08-30T10:00:00Z"));
        CHECK(v1.pse.type == QStringLiteral("image/jpeg"));
        CHECK(v1.acquisition.size() == 1);
        CHECK(v1.acquisition[0].href == QStringLiteral("http://komga.lan/opds/v1.2/books/0A1/file/saga-1.cbz"));
        CHECK(v1.coverHref == QStringLiteral("http://komga.lan/covers/0A1.png"));

        // The template resolved against the feed's base url AND kept its placeholder. A template that came
        // back percent-encoded (%7BpageNumber%7D) could never be substituted, and every page request would
        // fetch the literal template.
        CHECK(v1.pse.hrefTemplate
              == QStringLiteral("http://komga.lan/opds/v1.2/books/0A1/pages/{pageNumber}?zero_based=true"));

        // No pse:lastRead is NOT page 0: -1 means "the server has no position for you".
        CHECK(v2.pse.isValid());
        CHECK(v2.pse.count == 152);
        CHECK(v2.pse.lastRead == -1);
        CHECK(OpdsPse::lastReadIndex(v2.pse) == -1);

        // An entry with no PSE link offers none — every non-PSE server's rows are untouched.
        CHECK(!plain.pse.isValid());
        CHECK(plain.pse.count == 0);
        CHECK(plain.acquisition.size() == 1);
    }

    // The rel is matched by containment, so a feed that writes it bare (or in a rel list) still counts.
    CHECK(OpdsPse::isStreamRel(QStringLiteral("http://vaemendis.net/opds-pse/stream")));
    CHECK(OpdsPse::isStreamRel(QStringLiteral("opds-pse/stream")));
    CHECK(!OpdsPse::isStreamRel(QStringLiteral("http://opds-spec.org/acquisition")));

    // ================= 2. Template expansion ===========================================================
    {
        // WITHOUT a {maxWidth}: the server cannot resize, and the viewport width has nowhere to go. That
        // is not an error — it means full-size pages.
        OpdsPseLink k;
        k.hrefTemplate = QStringLiteral("http://komga.lan/opds/v1.2/books/0A1/pages/{pageNumber}?zero_based=true");
        k.count = 160;
        CHECK(!OpdsPse::supportsMaxWidth(k.hrefTemplate));
        CHECK(OpdsPse::firstPageNumber(k.hrefTemplate) == 0);
        CHECK(OpdsPse::pageUrl(k, 0, 900)
              == QStringLiteral("http://komga.lan/opds/v1.2/books/0A1/pages/0?zero_based=true"));
        CHECK(OpdsPse::pageUrl(k, 6, 900)
              == QStringLiteral("http://komga.lan/opds/v1.2/books/0A1/pages/6?zero_based=true"));

        // WITH a {maxWidth}: the viewport width goes in it.
        OpdsPseLink w;
        w.hrefTemplate = QStringLiteral("http://books.lan/comic/7/page/{pageNumber}?width={maxWidth}");
        w.count = 20;
        CHECK(OpdsPse::supportsMaxWidth(w.hrefTemplate));
        CHECK(OpdsPse::pageUrl(w, 3, 1440)
              == QStringLiteral("http://books.lan/comic/7/page/3?width=1440"));
        // A non-positive hint leaves the placeholder alone rather than asking for a zero-wide render.
        CHECK(OpdsPse::pageUrl(w, 3, 0)
              == QStringLiteral("http://books.lan/comic/7/page/3?width={maxWidth}"));

        // A server that declares zero_based=false is asked 1-based: page one is "1", not "0".
        OpdsPseLink one;
        one.hrefTemplate = QStringLiteral("http://books.lan/c/7/p/{pageNumber}?zero_based=false");
        one.count = 20;
        CHECK(OpdsPse::firstPageNumber(one.hrefTemplate) == 1);
        CHECK(OpdsPse::pageUrl(one, 0, 0) == QStringLiteral("http://books.lan/c/7/p/1?zero_based=false"));

        // A count with nowhere to put the page number is not a streamable offer.
        OpdsPseLink broken; broken.count = 30; broken.hrefTemplate = QStringLiteral("http://x.lan/page");
        CHECK(!broken.isValid());
        OpdsPseLink empty;  empty.hrefTemplate = QStringLiteral("http://x.lan/{pageNumber}");
        CHECK(!empty.isValid());       // count 0
    }

    // ================= 3. Resume: consuming pse:lastRead ===============================================
    {
        OpdsPseLink k; k.hrefTemplate = QStringLiteral("http://x.lan/p/{pageNumber}"); k.count = 160;
        k.lastRead = 7;
        CHECK(OpdsPse::lastReadIndex(k) == 6);      // "you got to page 7" -> the index of page 7
        k.lastRead = 1;  CHECK(OpdsPse::lastReadIndex(k) == 0);
        k.lastRead = 0;  CHECK(OpdsPse::lastReadIndex(k) == 0);   // clamped, never negative
        k.lastRead = 900; CHECK(OpdsPse::lastReadIndex(k) == 159); // past the end -> the last page
        k.lastRead = -1; CHECK(OpdsPse::lastReadIndex(k) == -1);   // absent -> say nothing
    }

    // ================= 4. Prefetch order ===============================================================
    {
        const QVector<int> ord = OpdsPse::fetchOrder(10, 6);
        CHECK(ord.size() == 10);
        CHECK(ord.mid(0, 3) == QVector<int>({ 6, 7, 8 }));            // the resume page, then N+1, N+2
        CHECK(ord.mid(3) == QVector<int>({ 0, 1, 2, 3, 4, 5, 9 }));   // then the rest, in reading order
        QVector<int> seen = ord; std::sort(seen.begin(), seen.end());
        QVector<int> all; for (int i = 0; i < 10; ++i) all.push_back(i);
        CHECK(seen == all);                                           // every page exactly once

        // Near the end there are fewer than two pages after N, and nothing is invented past the last one.
        CHECK(OpdsPse::fetchOrder(4, 3) == QVector<int>({ 3, 0, 1, 2 }));
        // No resume, or a start out of range: plain reading order.
        CHECK(OpdsPse::fetchOrder(4, -1) == QVector<int>({ 0, 1, 2, 3 }));
        CHECK(OpdsPse::fetchOrder(4, 99) == QVector<int>({ 0, 1, 2, 3 }));
        CHECK(OpdsPse::fetchOrder(0, 0).isEmpty());
    }

    // ================= 5. The page list, and the credentials on it =====================================
    {
        OpdsPseLink k;
        k.hrefTemplate = QStringLiteral("http://komga.lan/opds/v1.2/books/0A1/pages/{pageNumber}?zero_based=true");
        k.count = 3;
        // The header VALUE here is an opaque sentinel, deliberately NOT a real "Basic base64(user:pass)"
        // string. What this section measures is that whatever the caller handed in arrives on every page
        // unchanged and in the HEADERS — the value's internal shape is irrelevant to that, and a file in
        // this repository carrying a credential-shaped literal is a finding for every secret scanner that
        // reads it. opdsBasicAuth's actual encoding is pinned in probe_opds, against a hand-computed
        // oracle, which is where that question belongs.
        StreamHeaders::Headers auth;
        auth.insert(QStringLiteral("Authorization"), QStringLiteral("Basic EB-PSE-PROBE-SENTINEL"));
        const QVector<AddonPage> pages = OpdsPse::pageList(k, 900, auth);
        CHECK(pages.size() == 3);
        if (pages.size() == 3)
        {
            CHECK(pages[0].url == QStringLiteral("http://komga.lan/opds/v1.2/books/0A1/pages/0?zero_based=true"));
            CHECK(pages[2].url == QStringLiteral("http://komga.lan/opds/v1.2/books/0A1/pages/2?zero_based=true"));
            // The catalog's credentials are on EVERY page request, exactly as on the feed request.
            for (const AddonPage& p : pages)
                CHECK(p.headers.value(QStringLiteral("Authorization"))
                      == QStringLiteral("Basic EB-PSE-PROBE-SENTINEL"));
        }
        OpdsPseLink none;
        CHECK(OpdsPse::pageList(none, 900, auth).isEmpty());   // no offer -> no pages
    }

    // ================= 6. Progress: the two shapes, and no third ======================================
    {
        // Komga: PATCH <base>/api/v1/books/<id>/read-progress, 1-based page, completed on the last one.
        OpdsPseLink k;
        k.hrefTemplate = QStringLiteral("http://komga.lan/opds/v1.2/books/0A1/pages/{pageNumber}?zero_based=true");
        k.count = 160;
        const OpdsPse::PseProgress p6 = OpdsPse::progressReport(k, 6);
        CHECK(p6.isValid());
        CHECK(p6.server == QStringLiteral("komga"));
        CHECK(p6.method == QStringLiteral("PATCH"));
        CHECK(p6.url == QStringLiteral("http://komga.lan/api/v1/books/0A1/read-progress"));
        CHECK(p6.body == QByteArray("{\"page\":7,\"completed\":false}"));
        const OpdsPse::PseProgress plast = OpdsPse::progressReport(k, 159);
        CHECK(plast.body == QByteArray("{\"page\":160,\"completed\":true}"));

        // ...behind a reverse proxy on a sub-path, and on a non-default port.
        OpdsPseLink sub;
        sub.hrefTemplate = QStringLiteral("https://home.lan:8443/komga/opds/v1.2/books/9Z/pages/{pageNumber}");
        sub.count = 10;
        const OpdsPse::PseProgress ps = OpdsPse::progressReport(sub, 0);
        CHECK(ps.url == QStringLiteral("https://home.lan:8443/komga/api/v1/books/9Z/read-progress"));

        // Kavita: POST <base>/api/reader/progress with the ids its href already carries.
        OpdsPseLink kv;
        kv.hrefTemplate = QStringLiteral("http://kavita.lan/api/opds/APIKEY/image"
                                         "?libraryId=1&seriesId=2&volumeId=3&chapterId=4&pageNumber={pageNumber}");
        kv.count = 40;
        const OpdsPse::PseProgress pk = OpdsPse::progressReport(kv, 11);
        CHECK(pk.isValid());
        CHECK(pk.server == QStringLiteral("kavita"));
        CHECK(pk.method == QStringLiteral("POST"));
        CHECK(pk.url == QStringLiteral("http://kavita.lan/api/reader/progress"));
        CHECK(pk.body == QByteArray("{\"libraryId\":1,\"seriesId\":2,\"volumeId\":3,\"chapterId\":4,\"pageNum\":11}"));

        // A Kavita-shaped href with no chapterId names nothing to report against: send nothing.
        OpdsPseLink kvbad;
        kvbad.hrefTemplate = QStringLiteral("http://kavita.lan/api/opds/APIKEY/image?pageNumber={pageNumber}");
        kvbad.count = 40;
        CHECK(!OpdsPse::progressReport(kvbad, 3).isValid());

        // A server this client does not recognise: NOTHING is sent. #153 — do not invent an endpoint.
        OpdsPseLink other;
        other.hrefTemplate = QStringLiteral("http://ubooquity.lan/comics/7/page/{pageNumber}");
        other.count = 40;
        CHECK(!OpdsPse::progressReport(other, 3).isValid());
        // ...and neither is anything sent before there is a page to report.
        CHECK(!OpdsPse::progressReport(k, -1).isValid());
    }

    // ================= 7. The credential never leaves the header ======================================
    {
        // The tripwire, not a computed value: a page url and a progress request are both built with a
        // secret-shaped sentinel in the headers, and it must appear in NEITHER. It is the whole reason
        // pageList takes headers rather than a signed url. (A SENTINEL and not a credential, for the
        // reason given in section 5: nothing that looks like one belongs in a committed file.)
        const QByteArray secret = "EB-PSE-PROBE-SENTINEL";
        const QString basic = QStringLiteral("Basic EB-PSE-PROBE-SENTINEL");
        StreamHeaders::Headers auth;
        auth.insert(QStringLiteral("Authorization"), basic);
        OpdsPseLink k;
        k.hrefTemplate = QStringLiteral("http://komga.lan/opds/v1.2/books/0A1/pages/{pageNumber}?zero_based=true");
        k.count = 5;
        for (const AddonPage& p : OpdsPse::pageList(k, 900, auth))
        {
            CHECK(!p.url.toUtf8().contains(secret));
            CHECK(!p.url.contains(basic));
        }
        const OpdsPse::PseProgress rep = OpdsPse::progressReport(k, 2);
        CHECK(!rep.url.toUtf8().contains(secret));
        CHECK(!rep.body.contains(secret));
        CHECK(!rep.url.contains(basic));
    }

    // ================= 8. The seam's defaults are the pre-#153 behaviour ==============================
    {
        // PageSupplyOptions exists so a supplier can ask the page seam for four things; a caller that asks
        // for none of them must get exactly what the seam did before it existed, or #188's addon pages and
        // every folder run change behaviour by accident.
        const PageSupplyOptions dflt;
        CHECK(dflt.fetchOrder.isEmpty());     // plain reading order
        CHECK(!dflt.requireAllPages);         // a missing page is skipped, the chapter still opens
        CHECK(dflt.startPage0 == -1);         // land wherever the reader would have landed
        CHECK(!dflt.onIncomplete);            // the seam's own toast owns a failure
    }

    if (failures == 0)
        std::printf("PSE-OK\n");
    return failures == 0 ? 0 : 1;
}
