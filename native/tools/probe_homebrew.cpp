// Headless check of the homebrew client (src/core/HomebrewClient) — the URL a console's Homebrew folder asks
// for, the parse of what comes back, and the two level markers that let Back rebuild the folder.
//
// No network and no process: every input here is a hand-written byte array, which is the whole reason the
// parsing lives in a namespace of free functions rather than inside the view. What it pins:
//
//   * listUrl percent-encodes the system id as ONE path segment, so an id shaped like a URL becomes a
//     segment on our own server rather than a request to somewhere else;
//   * a base URL with trailing slashes is not doubled;
//   * a cursor is appended verbatim (percent-encoded) and omitted entirely when there is none — the cursor is
//     opaque to this client, so it is carried, never parsed;
//   * a valid body yields its rows and carries nextCursor; a null nextCursor means "no more";
//   * a body that is NOT the expected object — an error page, a challenge body, `[]`, `null`, garbage —
//     yields no rows and does not crash, the same policy RomhackClient::parseList states;
//   * a row with an EMPTY id is dropped, and its siblings survive: a row that could never be played is not
//     worth offering, mirroring RomhackClient's rule;
//   * the level marker ("homebrew:<system>") and the paging marker round-trip, INCLUDING a cursor holding the
//     separators and a base holding a colon — those markers are what loadTop() reads to repopulate on Back,
//     so a marker that cannot be read back is a level that cannot survive Back.
//
// Prints HOMEBREW-OK on success; any failure prints HOMEBREW-FAIL <cond> (line) and exits non-zero.
#include "HomebrewClient.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QString>
#include <cstdio>

static int g_fails = 0;

#define CHECK(cond)                                                                     \
    do {                                                                                \
        if (!(cond)) { std::printf("HOMEBREW-FAIL %s (%d)\n", #cond, __LINE__); ++g_fails; } \
    } while (0)

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // ---- listUrl -----------------------------------------------------------------------------------------
    {
        CHECK(HomebrewClient::listUrl(QStringLiteral("https://h/tok"), QStringLiteral("nds"))
              == QStringLiteral("https://h/tok/homebrew/nds"));
        // A stored base URL commonly ends in '/'. It must not become "//homebrew".
        CHECK(HomebrewClient::listUrl(QStringLiteral("https://h/tok/"), QStringLiteral("nds"))
              == QStringLiteral("https://h/tok/homebrew/nds"));
        CHECK(HomebrewClient::listUrl(QStringLiteral("https://h/tok///"), QStringLiteral("nds"))
              == QStringLiteral("https://h/tok/homebrew/nds"));

        // THE injection guard: the system id is ONE percent-encoded segment on our own server.
        const QString hostile = HomebrewClient::listUrl(QStringLiteral("https://h/tok"),
                                                        QStringLiteral("https://evil.example/x"));
        CHECK(hostile.startsWith(QStringLiteral("https://h/tok/homebrew/")));
        CHECK(!hostile.contains(QStringLiteral("evil.example/x")));   // the slashes are encoded away

        // A cursor rides as a query parameter, percent-encoded, and is absent entirely when empty.
        CHECK(HomebrewClient::listUrl(QStringLiteral("https://h/tok"), QStringLiteral("nds"),
                                      QString())
              == QStringLiteral("https://h/tok/homebrew/nds"));
        CHECK(!HomebrewClient::listUrl(QStringLiteral("https://h/tok"), QStringLiteral("nds"), QString())
               .contains(QLatin1Char('?')));
        CHECK(HomebrewClient::listUrl(QStringLiteral("https://h/tok"), QStringLiteral("nds"),
                                      QStringLiteral("page|12 3"))
              == QStringLiteral("https://h/tok/homebrew/nds?cursor=page%7C12%203"));
        // An opaque cursor can hold anything at all; it is carried, never interpreted.
        const QString weird = HomebrewClient::listUrl(QStringLiteral("https://h/tok"), QStringLiteral("gba"),
                                                      QStringLiteral("a&b=c#d/e"));
        CHECK(weird.startsWith(QStringLiteral("https://h/tok/homebrew/gba?cursor=")));
        CHECK(!weird.contains(QLatin1Char('&')));
        CHECK(!weird.contains(QLatin1Char('#')));
    }

    // ---- parseList: the good body -------------------------------------------------------------------------
    {
        const QByteArray body =
            "{\"items\":["
            "{\"id\":\"retro:gb:abc\",\"title\":\"Dust\",\"author\":\"Ana\",\"version\":\"1.2\","
            "\"description\":\"a demo\",\"imageUrl\":\"https://i/1.png\"},"
            "{\"id\":\"retro:gb:def\",\"title\":\"Ember\",\"author\":null,\"version\":null,"
            "\"description\":null,\"imageUrl\":null}"
            "],\"nextCursor\":\"page|2\"}";
        const HomebrewPage p = HomebrewClient::parseList(body);
        CHECK(p.items.size() == 2);
        if (p.items.size() == 2)
        {
            CHECK(p.items[0].id == QStringLiteral("retro:gb:abc"));
            CHECK(p.items[0].title == QStringLiteral("Dust"));
            CHECK(p.items[0].author == QStringLiteral("Ana"));
            CHECK(p.items[0].version == QStringLiteral("1.2"));
            CHECK(p.items[0].description == QStringLiteral("a demo"));
            CHECK(p.items[0].imageUrl == QStringLiteral("https://i/1.png"));
            // A source that states nothing but a title still gives a usable row: the optional fields are
            // empty, never the string "null".
            CHECK(p.items[1].id == QStringLiteral("retro:gb:def"));
            CHECK(p.items[1].author.isEmpty());
            CHECK(p.items[1].version.isEmpty());
            CHECK(p.items[1].imageUrl.isEmpty());
            // The one-line label: title, then whatever else the source said. Never "Ember ( · )".
            CHECK(p.items[0].subtitle() == QStringLiteral("Ana · v1.2"));
            CHECK(p.items[1].subtitle().isEmpty());
        }
        CHECK(p.nextCursor == QStringLiteral("page|2"));
        CHECK(p.hasMore());
    }

    // A last page says so by carrying no cursor — both spellings the server may use.
    {
        const HomebrewPage p = HomebrewClient::parseList(
            "{\"items\":[{\"id\":\"x\",\"title\":\"T\"}],\"nextCursor\":null}");
        CHECK(p.items.size() == 1);
        CHECK(p.nextCursor.isEmpty());
        CHECK(!p.hasMore());
        const HomebrewPage q = HomebrewClient::parseList("{\"items\":[{\"id\":\"x\",\"title\":\"T\"}]}");
        CHECK(q.items.size() == 1);
        CHECK(!q.hasMore());
    }

    // A plugin-less server: 200 with an empty page. Not an error, just an empty folder.
    {
        const HomebrewPage p = HomebrewClient::parseList("{\"items\":[],\"nextCursor\":null}");
        CHECK(p.items.isEmpty());
        CHECK(!p.hasMore());
    }

    // ---- parseList: everything that is not the expected object --------------------------------------------
    {
        // Each of these is something a proxy, a captive portal or a bot-challenge really does return in place
        // of the body we asked for. None of them is a crash and none of them is a row.
        const char* junk[] = {
            "",
            "   ",
            "null",
            "[]",
            "[{\"id\":\"x\",\"title\":\"T\"}]",              // the ROMHACK shape: an array, not our object
            "\"a string\"",
            "42",
            "<!DOCTYPE html><html><body>403</body></html>",
            "{",
            "{\"items\":null}",
            "{\"items\":\"nope\"}",
            "{\"items\":{\"id\":\"x\"}}",
            "{\"error\":\"nope\"}",
        };
        for (const char* j : junk)
        {
            const HomebrewPage p = HomebrewClient::parseList(QByteArray(j));
            CHECK(p.items.isEmpty());
            CHECK(!p.hasMore());
        }
        // A non-string cursor is no cursor: paging on it would build a nonsense URL.
        CHECK(!HomebrewClient::parseList("{\"items\":[],\"nextCursor\":7}").hasMore());
    }

    // ---- parseList: a row that could never be played is dropped -------------------------------------------
    {
        const QByteArray body =
            "{\"items\":["
            "{\"id\":\"\",\"title\":\"No id\"},"                     // empty id: unplayable
            "{\"title\":\"Missing id\"},"                             // no id at all: the same thing
            "{\"id\":null,\"title\":\"Null id\"},"
            "\"not an object\","                                      // not even a row
            "{\"id\":\"retro:gb:keep\",\"title\":\"Keep\"}"
            "],\"nextCursor\":null}";
        const HomebrewPage p = HomebrewClient::parseList(body);
        // Exactly one survivor, and it is the one with an id — the bad rows are dropped, not the page.
        CHECK(p.items.size() == 1);
        if (p.items.size() == 1) CHECK(p.items[0].id == QStringLiteral("retro:gb:keep"));
        for (const HomebrewTitle& t : p.items) CHECK(!t.id.isEmpty());
    }

    // ---- the level markers: what Back reads back ----------------------------------------------------------
    {
        // The folder's level carries "homebrew:<system>" so loadTop() can rebuild it after a Back out of a
        // played title. Written in one place, read in another: if these two disagree, Back lands on nothing.
        CHECK(HomebrewClient::levelMime(QStringLiteral("nds")) == QStringLiteral("homebrew:nds"));
        CHECK(HomebrewClient::levelSystem(HomebrewClient::levelMime(QStringLiteral("psvita")))
              == QStringLiteral("psvita"));
        CHECK(HomebrewClient::levelSystem(QStringLiteral("favorites:nds")).isEmpty());  // another folder's marker
        CHECK(HomebrewClient::levelSystem(QString()).isEmpty());
    }

    // ---- the paging marker: a per-server continuation, carried verbatim ------------------------------------
    {
        QVector<HomebrewMore> more;
        more.push_back({ QStringLiteral("https://h/tok"), QStringLiteral("page|2") });
        // A second server with its own cursor: paging must follow BOTH, and neither cursor may be parsed.
        more.push_back({ QStringLiteral("http://other:7000/x"),
                         QStringLiteral("has:colon\tand\ttab\nand\nnewline") });
        const QString mime = HomebrewClient::moreMime(QStringLiteral("gba"), more);
        CHECK(mime.startsWith(QStringLiteral("homebrewmore:")));
        CHECK(HomebrewClient::moreSystem(mime) == QStringLiteral("gba"));
        const QVector<HomebrewMore> back = HomebrewClient::moreCursors(mime);
        CHECK(back.size() == 2);
        if (back.size() == 2)
        {
            CHECK(back[0].base == more[0].base);
            CHECK(back[0].cursor == more[0].cursor);
            CHECK(back[1].base == more[1].base);
            CHECK(back[1].cursor == more[1].cursor);   // separators survive: the cursor is opaque, not parsed
        }
        // No continuation, no marker content to read back.
        CHECK(HomebrewClient::moreCursors(HomebrewClient::moreMime(QStringLiteral("gba"), {})).isEmpty());
        CHECK(HomebrewClient::moreSystem(HomebrewClient::moreMime(QStringLiteral("gba"), {}))
              == QStringLiteral("gba"));
        // Another folder's marker is not ours.
        CHECK(HomebrewClient::moreCursors(QStringLiteral("homebrew:gba")).isEmpty());
        CHECK(HomebrewClient::moreSystem(QStringLiteral("homebrew:gba")).isEmpty());
    }

    if (g_fails == 0) std::printf("HOMEBREW-OK\n");
    return g_fails == 0 ? 0 : 1;
}
