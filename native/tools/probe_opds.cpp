// Headless check of the OPDS catalog feature's pure heart + store (#146):
//
//   * parseOpds (src/ebook/OpdsFeed.h) — parse a hand-authored OPDS 1.2 Atom feed: feed title, and per entry
//     the title / author / id, and the classification of each <link> into acquisition (downloadable book) vs
//     navigation (drill into a sub-feed) vs cover/thumbnail, by rel/type. Relative hrefs resolved against the
//     feed's base url; a malformed feed yields an empty feed and never throws.
//   * resolveHref — a relative href resolves against the base; an absolute href is left alone.
//   * opdsBasicAuth — "Basic <base64(user:pass)>" for creds, empty without a username.
//   * OpdsCatalogStore (src/core/OpdsCatalogStore) — a per-profile catalog list round-trips add/update/remove,
//     the username/password fields survive, and two profiles never cross.
//
// QtCore-only, so it runs under the offscreen QPA in CI. Prints OPDS-OK on success; any failure prints
// OPDS-FAIL <cond> (line) and exits non-zero.
//
// FIXTURE INDEPENDENCE: the Atom XML below is hand-written, and every expected value (which link is the
// acquisition, which is navigation, the resolved cover url, the base64 of the credentials) is computed by
// hand / by an independent oracle (python base64), NEVER by running parseOpds or opdsBasicAuth. So the parser
// is measured against an oracle that does not run it.
//
// Isolation: AppPaths::dataDir() is this process's own scratch directory (EB_ISOLATED_DATA_DIR), so the
// everythingbox.ini OpdsCatalogStore opens starts empty and is removed at exit.
#include "OpdsFeed.h"
#include "OpdsCatalogStore.h"
#include "ProfileStore.h"

#include <QCoreApplication>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "OPDS-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

// A well-formed OPDS 1.2 feed with one navigation entry (a sub-shelf) and one acquisition entry (a book with
// a full cover + a thumbnail + a downloadable epub). Base url is http://books.lan/opds/root.xml, so the
// relative hrefs below resolve against http://books.lan/opds/ .
static const char* kFeed = R"(<?xml version="1.0" encoding="utf-8"?>
<feed xmlns="http://www.w3.org/2005/Atom" xmlns:opds="http://opds-spec.org/2010/catalog"
      xmlns:pse="http://vaemendis.net/opds-pse/ns">
  <id>urn:root</id>
  <title>My Library</title>
  <link rel="self" href="/opds/root.xml" type="application/atom+xml;profile=opds-catalog;kind=navigation"/>
  <entry>
    <title>Science Fiction</title>
    <id>urn:shelf:sf</id>
    <updated>2026-01-01T00:00:00Z</updated>
    <link rel="subsection" href="shelf/sf.xml" type="application/atom+xml;profile=opds-catalog;kind=acquisition"/>
  </entry>
  <entry>
    <title>Dune</title>
    <id>urn:book:dune</id>
    <author><name>Frank Herbert</name></author>
    <summary>Desert planet.</summary>
    <link rel="http://opds-spec.org/image" href="/covers/dune.png" type="image/png"/>
    <link rel="http://opds-spec.org/image/thumbnail" href="/covers/dune-thumb.png" type="image/png"/>
    <link rel="http://opds-spec.org/acquisition" href="download/dune.epub" type="application/epub+zip"/>
  </entry>
  <entry>
    <title>Saga, Vol. 1</title>
    <id>urn:book:saga1</id>
    <link rel="http://opds-spec.org/acquisition" href="download/saga-1.cbz"
          type="application/vnd.comicbook+zip"/>
    <link rel="http://vaemendis.net/opds-pse/stream" href="books/0A1/pages/{pageNumber}?zero_based=true"
          type="image/jpeg" pse:count="160" pse:lastRead="7"/>
  </entry>
</feed>)";

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    const QString base = QStringLiteral("http://books.lan/opds/root.xml");

    // ================= 1. parseOpds: feed + entry fields =========================================
    {
        const OpdsFeed feed = parseOpds(QByteArray(kFeed), base);
        CHECK(feed.title == QStringLiteral("My Library"));
        CHECK(feed.id == QStringLiteral("urn:root"));
        CHECK(feed.entries.size() == 3);

        // --- entry 0: a NAVIGATION shelf (subsection, opds-catalog type) — no acquisition, no cover.
        const OpdsEntry& nav = feed.entries[0];
        CHECK(nav.title == QStringLiteral("Science Fiction"));
        CHECK(nav.id == QStringLiteral("urn:shelf:sf"));
        CHECK(nav.navigation.size() == 1);
        CHECK(nav.acquisition.isEmpty());
        CHECK(nav.coverHref.isEmpty());
        // the sub-feed href resolved against the base's directory
        CHECK(nav.navigation[0].href == QStringLiteral("http://books.lan/opds/shelf/sf.xml"));

        // --- entry 1: an ACQUISITION book — title/author, one acquisition link, cover prefers full over thumb.
        const OpdsEntry& book = feed.entries[1];
        CHECK(book.title == QStringLiteral("Dune"));
        CHECK(book.author == QStringLiteral("Frank Herbert"));
        CHECK(book.id == QStringLiteral("urn:book:dune"));
        CHECK(book.summary == QStringLiteral("Desert planet."));
        CHECK(book.acquisition.size() == 1);
        CHECK(book.navigation.isEmpty());
        // acquisition href resolved + mime type preserved (so the UI can prefer epub)
        CHECK(book.acquisition[0].href == QStringLiteral("http://books.lan/opds/download/dune.epub"));
        CHECK(book.acquisition[0].type == QStringLiteral("application/epub+zip"));
        // cover: the FULL image wins over the thumbnail, resolved to absolute (root-relative -> host root)
        CHECK(book.coverHref == QStringLiteral("http://books.lan/covers/dune.png"));
        // ...and an entry the server offered no page stream for offers none. -1, not 0: "no progress" and
        // "you are on the first page" are different statements (#153).
        CHECK(!book.pse.isValid());
        CHECK(book.pse.count == 0);
        CHECK(book.pse.lastRead == -1);

        // --- entry 2: an OPDS-PSE COMIC (#153). The page-stream link is lifted into `pse` and is NOT
        // mistaken for an acquisition — the download is still there, which is the whole "Read online
        // BESIDE Download" rule. Its href keeps its {pageNumber} placeholder through the resolve; a
        // template that came back percent-encoded could never be substituted.
        const OpdsEntry& comic = feed.entries[2];
        CHECK(comic.title == QStringLiteral("Saga, Vol. 1"));
        CHECK(comic.acquisition.size() == 1);
        CHECK(comic.acquisition[0].href == QStringLiteral("http://books.lan/opds/download/saga-1.cbz"));
        CHECK(comic.pse.isValid());
        CHECK(comic.pse.count == 160);
        CHECK(comic.pse.lastRead == 7);
        CHECK(comic.pse.type == QStringLiteral("image/jpeg"));
        CHECK(comic.pse.hrefTemplate
              == QStringLiteral("http://books.lan/opds/books/0A1/pages/{pageNumber}?zero_based=true"));
    }

    // ================= 2. resolveHref: relative resolves, absolute is left alone ==================
    {
        // a directory-relative href resolves against the base's directory
        CHECK(resolveHref(base, QStringLiteral("sub.xml")) == QStringLiteral("http://books.lan/opds/sub.xml"));
        // a root-relative href resolves against the host root
        CHECK(resolveHref(base, QStringLiteral("/x/y.png")) == QStringLiteral("http://books.lan/x/y.png"));
        // an already-absolute href is returned unchanged (NOT re-based)
        CHECK(resolveHref(base, QStringLiteral("https://cdn.example/z.epub"))
              == QStringLiteral("https://cdn.example/z.epub"));
        // an empty base leaves a relative href as-is (nothing to resolve against)
        CHECK(resolveHref(QString(), QStringLiteral("rel.xml")) == QStringLiteral("rel.xml"));
        // a TEMPLATED href (#153) resolves like any other AND keeps its braces: `{` and `}` are not legal
        // url characters, and a %7BpageNumber%7D coming back out would be unsubstitutable for ever.
        CHECK(resolveHref(base, QStringLiteral("b/1/pages/{pageNumber}?w={maxWidth}"))
              == QStringLiteral("http://books.lan/opds/b/1/pages/{pageNumber}?w={maxWidth}"));
    }

    // ================= 3. opdsBasicAuth: base64(user:pass), empty without a username ==============
    {
        // Independent oracle (python: base64.b64encode(b"reader:s3cr3t")) == "cmVhZGVyOnMzY3IzdA=="
        CHECK(opdsBasicAuth(QStringLiteral("reader"), QStringLiteral("s3cr3t"))
              == QStringLiteral("Basic cmVhZGVyOnMzY3IzdA=="));
        // no username -> no header (an open catalog sends no Authorization)
        CHECK(opdsBasicAuth(QString(), QStringLiteral("s3cr3t")).isEmpty());
    }

    // ================= 4. malformed feed -> empty, never throws ===================================
    {
        const OpdsFeed bad = parseOpds(QByteArray("<feed><entry><title>oops"), base); // truncated, unclosed
        CHECK(bad.entries.isEmpty());   // no <entry> ever closed, so best-effort yields nothing
        CHECK(bad.title.isEmpty());
        const OpdsFeed empty = parseOpds(QByteArray("not xml at all {}"), base);
        CHECK(empty.entries.isEmpty());
        const OpdsFeed none = parseOpds(QByteArray(), base);
        CHECK(none.entries.isEmpty());  // empty input is fine
    }

    // ================= 5. OpdsCatalogStore per-profile round-trip ================================
    ProfileStore::setCurrent(QStringLiteral("probeOpdsA"));
    CHECK(OpdsCatalogStore::list().isEmpty());   // a fresh profile has no catalogs

    OpdsCatalog c1;
    c1.name = QStringLiteral("My Calibre");
    c1.url = QStringLiteral("http://books.lan/opds");
    c1.username = QStringLiteral("reader");
    c1.password = QStringLiteral("s3cr3t");       // a NON-empty password to prove it round-trips
    const QString id1 = OpdsCatalogStore::add(c1);
    CHECK(!id1.isEmpty());                          // add mints a stable id

    OpdsCatalog c2;
    c2.name = QStringLiteral("Open Server");
    c2.url = QStringLiteral("http://open.lan/opds"); // no creds (the open-catalog case)
    const QString id2 = OpdsCatalogStore::add(c2);
    CHECK(!id2.isEmpty() && id2 != id1);

    {
        const QList<OpdsCatalog> all = OpdsCatalogStore::list();
        CHECK(all.size() == 2);
        OpdsCatalog got;
        CHECK(OpdsCatalogStore::get(id1, got));
        CHECK(got.name == QStringLiteral("My Calibre"));
        CHECK(got.url == QStringLiteral("http://books.lan/opds"));
        CHECK(got.username == QStringLiteral("reader"));
        CHECK(got.password == QStringLiteral("s3cr3t"));   // credential survives a round-trip
        OpdsCatalog got2;
        CHECK(OpdsCatalogStore::get(id2, got2));
        CHECK(got2.username.isEmpty() && got2.password.isEmpty()); // open catalog stays open
    }

    // update: replace fields for id1, disturbing nothing else.
    {
        OpdsCatalog up; up.id = id1; up.name = QStringLiteral("Renamed");
        up.url = QStringLiteral("http://books.lan/opds2"); up.username = QStringLiteral("u2");
        up.password = QStringLiteral("p2");
        OpdsCatalogStore::update(up);
        OpdsCatalog got;
        CHECK(OpdsCatalogStore::get(id1, got));
        CHECK(got.name == QStringLiteral("Renamed"));
        CHECK(got.url == QStringLiteral("http://books.lan/opds2"));
        CHECK(got.username == QStringLiteral("u2"));
        CHECK(got.password == QStringLiteral("p2"));
        CHECK(OpdsCatalogStore::list().size() == 2);   // update never adds/removes
    }

    // remove: drops exactly id1; id2 is untouched.
    {
        OpdsCatalogStore::remove(id1);
        OpdsCatalog gone;
        CHECK(!OpdsCatalogStore::get(id1, gone));
        CHECK(OpdsCatalogStore::list().size() == 1);
    }

    // a second profile never sees the first profile's catalogs.
    {
        ProfileStore::setCurrent(QStringLiteral("probeOpdsB"));
        CHECK(OpdsCatalogStore::list().isEmpty());
        OpdsCatalog cb; cb.name = QStringLiteral("B only"); cb.url = QStringLiteral("http://b.lan/opds");
        OpdsCatalogStore::add(cb);
        CHECK(OpdsCatalogStore::list().size() == 1);
        ProfileStore::setCurrent(QStringLiteral("probeOpdsA"));
        CHECK(OpdsCatalogStore::list().size() == 1);   // still just id2, unaffected by B
    }

    if (failures == 0)
        std::printf("OPDS-OK\n");
    return failures == 0 ? 0 : 1;
}
