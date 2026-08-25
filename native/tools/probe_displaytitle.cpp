// Headless check of THE DISPLAY RULE — what may be put on screen as the name of a track (issue #202).
//
// The bug this pins, three times over: a Subsonic stream url IS a credential (the query carries `u`, the
// salted token `t` and the salt `s`), and `QFileInfo(url).completeBaseName()` treats a url as a filesystem
// path, so the "base name" of a stream url is a slice of its QUERY. The same idiom had already been fixed in
// the resume/stats titles (#193 increment 5) and in the recents store and uitest key (#200); #202 is the
// screen itself. DisplayTitle is the rule that ends the family.
//
// EVERY assertion here comes in a PAIR, and that is the whole design of this file:
//
//     NOLEAK — no invented secret below survives into any label, from any shape of url.
//     KEEP   — a legitimate title is returned BYTE FOR BYTE, including one with a '?' in it.
//
// A rule that returns "" for everything passes every NOLEAK on its own, and a rule that returns its input
// passes every KEEP. Neither is the feature. native/tools/displaytitle-mutants.json breaks it in both
// directions for exactly that reason — an over-eager sanitiser that eats "Who Framed Roger Rabbit?" is its
// own bug, and a worse one than the leak, because it is silent and it fires every day.
//
// THE TOKENS BELOW ARE INVENTED. Nothing in this file, in the mutants matrix or in the report is a real
// credential; a real one is live in the maintainer's install and never goes in a fixture.
//
// Prints DISPLAYTITLE-OK on success; any failure prints DISPLAYTITLE-FAIL <cond> and exits non-zero.
#include "DisplayTitle.h"

#include <QCoreApplication>
#include <QString>
#include <QStringList>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "DISPLAYTITLE-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

// The invented secrets. Every one is checked for by NOLEAK against every label this file produces, so a
// future change that starts letting a query through fails here rather than on someone's screenshot.
static const char* const kSecrets[] = {
    "e5f61c9d2a7b40338fa1", // the fake salted token
    "9q4zt1",               // the fake salt
    "hunter2",              // a fake plaintext password, for the userinfo and path-credential shapes
    "sk-fake-000111",       // a fake api key
};

// No invented secret, and no fragment of one, appears anywhere in `label`. Substring, not equality: a rule
// that truncated a url would still be leaking, and "a token in the first 40 characters is still a token".
static bool noLeak(const QString& label)
{
    for (const char* s : kSecrets)
        if (label.contains(QLatin1String(s))) return false;
    return true;
}
#define NOLEAK(expr) do { \
    const QString _l = (expr); \
    if (!noLeak(_l)) { std::fprintf(stderr, "DISPLAYTITLE-FAIL leak in %s (line %d)\n", #expr, __LINE__); \
                       ++failures; } \
} while (0)

using DisplayTitle::choose;
using DisplayTitle::isUsable;
using DisplayTitle::fromLocation;

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // A signed Subsonic stream url, the exact shape that was on screen. The id ends in ".view" so that the
    // last dot before the query is NOT the last dot in the string — which is what made completeBaseName()'s
    // output depend on the server's id format rather than on anything anyone could reason about.
    const QString kSigned = QStringLiteral(
        "https://music.example.com/rest/stream.view?id=tr-4821&u=listener&t=e5f61c9d2a7b40338fa1&s=9q4zt1&v=1.16.1&c=EB");

    // =====================================================================================================
    // 1. THE NINE SHAPES THE BRIEF NAMES, each with the label it must produce
    // =====================================================================================================
    {
        // (a) a local path, with spaces in it. Its base name, exactly as it has always been — this is the
        //     case that must NOT change, and the one a careless fix breaks. (b) the same for a path with a
        //     host-specific shape: a UNC share on Windows, a hidden dot-directory on POSIX.
        //
        //     BOTH ARE HOST-SPECIFIC BY NATURE and that is why they are written twice (issue #205). The
        //     derivation ends in QFileInfo::completeBaseName(), and QFileInfo splits on the separators of
        //     the host it is running on: '\' is a separator on Windows and an ORDINARY, LEGAL CHARACTER in
        //     a POSIX file name. So "C:\Users\me\a b.mkv" names a file on Windows and is one 22-character
        //     file name on Linux, and asserting the Windows answer there would be asserting that QFileInfo
        //     mis-parses a legal name. The Windows assertions below are untouched on Windows; the POSIX
        //     ones state the same rule in the spelling that host actually uses.
#ifdef Q_OS_WIN
        CHECK(choose(QString(), QStringLiteral("C:\\Users\\me\\My Videos\\a b.mkv")) == QStringLiteral("a b"));
        CHECK(choose(QString(), QStringLiteral("\\\\server\\share\\Music\\My Song.flac"))
              == QStringLiteral("My Song"));
#else
        CHECK(choose(QString(), QStringLiteral("/home/me/My Videos/a b.mkv")) == QStringLiteral("a b"));
        CHECK(choose(QString(), QStringLiteral("/mnt/server/share/Music/My Song.flac"))
              == QStringLiteral("My Song"));
        // …and the backslash path, HERE, is a file name and not a path — the whole of it is the label,
        // which is the correct answer on this host and the reason the block above is guarded.
        CHECK(choose(QString(), QStringLiteral("C:\\Users\\me\\My Videos\\a b.mkv"))
              == QStringLiteral("C:\\Users\\me\\My Videos\\a b"));
#endif
        // (c) a file:// url — a scheme, but not a NETWORK scheme, so it is a file and is named like one.
        CHECK(choose(QString(), QStringLiteral("file:///C:/Music/Song.mp3")) == QStringLiteral("Song"));
        // (d) an http url with no query at all. The last segment of its PATH — deliberately the same answer
        //     the stores give (see the header note: one derivation, not two).
        CHECK(choose(QString(), QStringLiteral("http://cdn.example.com/songs/Fly.mp3"))
              == QStringLiteral("Fly.mp3"));
        // (e) the signed url. Its path's last segment, and NOT ONE CHARACTER of the query.
        CHECK(choose(QString(), kSigned) == QStringLiteral("stream.view"));
        NOLEAK(choose(QString(), kSigned));
        //     …and the last-resort derivation on its own, so the two are pinned separately and a change to
        //     the CHOICE cannot quietly become a change to the DERIVATION.
        CHECK(fromLocation(kSigned) == QStringLiteral("stream.view"));
#ifdef Q_OS_WIN
        CHECK(fromLocation(QStringLiteral("C:\\Users\\me\\My Videos\\a b.mkv")) == QStringLiteral("a b"));
#else
        CHECK(fromLocation(QStringLiteral("/home/me/My Videos/a b.mkv")) == QStringLiteral("a b"));
#endif
        // (f) a url whose LAST DOT falls inside the query — the accident completeBaseName() turned into a
        //     leak. Here the old idiom would have returned "view?id=9&u=listener&t=e5f61c9d2a7b40338fa1&s=9q4zt1&v"
        //     (everything after the last '/' up to the last '.'), i.e. the whole token.
        const QString lastDotInQuery = QStringLiteral(
            "https://music.example.com/rest/stream.view?id=9&u=listener&t=e5f61c9d2a7b40338fa1&s=9q4zt1&v=1.16.1");
        CHECK(choose(QString(), lastDotInQuery) == QStringLiteral("stream.view"));
        NOLEAK(choose(QString(), lastDotInQuery));
        // (g) credentials in the PATH rather than the query — the Xtream/IPTV shape. Only the LAST segment
        //     is ever the label, so the user and the password sitting in the middle of this path do not
        //     reach the screen. That is not the same as claiming path credentials are handled — see the
        //     tripwire immediately below, which says exactly what is and is not being claimed.
        const QString pathCreds = QStringLiteral("http://iptv.example.com/live/listener/hunter2/12345.ts");
        CHECK(choose(QString(), pathCreds) == QStringLiteral("12345.ts"));
        NOLEAK(choose(QString(), pathCreds));
        // THE DOCUMENTED LIMIT, asserted so that it is a decision rather than an accident: a credential that
        // is the WHOLE LAST PATH SEGMENT is not removed — here, or in StoredUrl, for the same reasons. The
        // path is what makes a row re-openable, no heuristic can tell a token segment from a content id, and
        // a wrong guess mangles every legitimate stream name. No source in this tree produces the shape: a
        // debrid link ends in the file name, a Subsonic link in "stream.view", an IPTV link in the channel
        // id. If one ever does, THIS assertion is what has to be argued with, rather than the behaviour
        // being rediscovered on someone's screen.
        const QString tokenLast = QStringLiteral("https://dl.example.com/stream/sk-fake-000111");
        CHECK(choose(QString(), tokenLast) == QStringLiteral("sk-fake-000111"));
        // (h) nothing at all. Empty in, empty out — a caller with nothing to say shows nothing rather than a
        //     placeholder that would be a lie, and the overlay's own visibility test does the rest.
        CHECK(choose(QString(), QString()).isEmpty());
        CHECK(choose(QString(), QString(), QString()).isEmpty());
        // (i) A REAL TITLE THAT LOOKS URL-ISH. Byte for byte, all of them.
        CHECK(choose(QStringLiteral("Who Framed Roger Rabbit?"), kSigned)
              == QStringLiteral("Who Framed Roger Rabbit?"));
        CHECK(choose(QStringLiteral("Whose Line Is It Anyway? The Movie"), kSigned)
              == QStringLiteral("Whose Line Is It Anyway? The Movie"));
        CHECK(choose(QStringLiteral("www.example.com"), kSigned) == QStringLiteral("www.example.com"));
        CHECK(choose(QStringLiteral("Episode 3: What? (2019)"), kSigned)
              == QStringLiteral("Episode 3: What? (2019)"));
        CHECK(choose(QStringLiteral("AC/DC — Back in Black"), kSigned) == QStringLiteral("AC/DC — Back in Black"));
        CHECK(choose(QStringLiteral("Re: //something"), kSigned) == QStringLiteral("Re: //something"));
        CHECK(choose(QStringLiteral("?"), kSigned) == QStringLiteral("?"));
        CHECK(choose(QStringLiteral("2 + 2 = 5"), kSigned) == QStringLiteral("2 + 2 = 5"));
    }

    // =====================================================================================================
    // 2. isUsable — the two grounds for rejecting a candidate, and the long list of NON-grounds
    // =====================================================================================================
    {
        // Rejected: it carries a url scheme. However clean the url is, a url is not a song's name.
        CHECK(!isUsable(kSigned));
        CHECK(!isUsable(QStringLiteral("https://music.example.com/rest/stream.view")));   // no query, still a url
        CHECK(!isUsable(QStringLiteral("file:///C:/Music/Song.mp3")));
        CHECK(!isUsable(QStringLiteral("rtsp://cam.example.com/1")));
        // Rejected: a QUERY TAIL on a bare label — the #200 shape, a completeBaseName() slice left in a title
        // field by an older build. No scheme, so nothing that reasons about urls would look twice at it.
        CHECK(!isUsable(QStringLiteral("7f3a9c21?t=e5f61c9d2a7b40338fa1&s=9q4zt1")));
        NOLEAK(choose(QStringLiteral("7f3a9c21?t=e5f61c9d2a7b40338fa1&s=9q4zt1"), kSigned));
        // Rejected: nothing.
        CHECK(!isUsable(QString()));
        // ACCEPTED — every one of these is a title, and a rule that ate any of them would be the worse bug.
        CHECK(isUsable(QStringLiteral("Who Framed Roger Rabbit?")));
        CHECK(isUsable(QStringLiteral("Whose Line Is It Anyway? The Movie")));
        CHECK(isUsable(QStringLiteral("www.example.com")));
        CHECK(isUsable(QStringLiteral("Episode 3: What? (2019)")));
        CHECK(isUsable(QStringLiteral("C:\\Users\\me\\My Videos\\a b.mkv")));  // odd, but not a leak
        CHECK(isUsable(QStringLiteral("Symphony No. 5")));
        CHECK(isUsable(QStringLiteral("Nine Inch Nails - 999,999")));
    }

    // =====================================================================================================
    // 3. THE CHOICE — first usable candidate wins, and a url candidate is SKIPPED rather than cleaned
    // =====================================================================================================
    {
        // The mpv site's exact shape: mpv's media-title first (an ICY song name or a real tag), the host's
        // queue title second, the location last.
        CHECK(choose(QStringLiteral("Bohemian Rhapsody"), QStringLiteral("Queue Title"), kSigned)
              == QStringLiteral("Bohemian Rhapsody"));
        // mpv fell back to the url: skip it, do NOT scrub it. StoredUrl::label of the signed url is
        // "https://music.example.com/rest/stream.view" — safe, and a terrible thing to call a song. THIS is
        // the assertion that distinguishes the display rule from the storage rule.
        CHECK(choose(kSigned, QStringLiteral("Queue Title"), kSigned) == QStringLiteral("Queue Title"));
        NOLEAK(choose(kSigned, QStringLiteral("Queue Title"), kSigned));
        // Both candidates unusable: the location, reduced.
        CHECK(choose(kSigned, QString(), kSigned) == QStringLiteral("stream.view"));
        CHECK(choose(kSigned, kSigned, kSigned) == QStringLiteral("stream.view"));
        NOLEAK(choose(kSigned, kSigned, kSigned));
        // The second candidate is consulted only when the first is unusable, never merged with it.
        CHECK(choose(QStringLiteral("First"), QStringLiteral("Second"), kSigned) == QStringLiteral("First"));
        CHECK(choose(QString(), QStringLiteral("Second"), kSigned) == QStringLiteral("Second"));
        // A local queue: the file's own base name still reaches the screen through the last resort. Spelled
        // per host for the reason given at (a) above — '\' is not a separator on POSIX.
#ifdef Q_OS_WIN
        CHECK(choose(QString(), QString(), QStringLiteral("D:\\Music\\Abbey Road\\01 Come Together.flac"))
              == QStringLiteral("01 Come Together"));
#else
        CHECK(choose(QString(), QString(), QStringLiteral("/media/Music/Abbey Road/01 Come Together.flac"))
              == QStringLiteral("01 Come Together"));
#endif
    }

    // =====================================================================================================
    // 4. USERINFO, PORTS, FRAGMENTS and the malformed shapes — nothing past the path ever reaches a label
    // =====================================================================================================
    {
        // user:pass@ is the one path-adjacent credential with an unambiguous syntax, and it goes.
        const QString userinfo = QStringLiteral("http://listener:hunter2@stream.example.com:4533/rest/stream");
        CHECK(choose(QString(), userinfo) == QStringLiteral("stream"));
        NOLEAK(choose(QString(), userinfo));
        // lastIndexOf('@'), so a password containing an encoded '@' cannot leave its tail in the host slot.
        // Only observable when the url has no path at all and the host itself becomes the label.
        const QString atInPass = QStringLiteral("http://listener:hunter2%40x@stream.example.com");
        CHECK(choose(QString(), atInPass) == QStringLiteral("stream.example.com"));
        NOLEAK(choose(QString(), atInPass));
        // A fragment before a query, and a query with no path at all.
        NOLEAK(choose(QString(), QStringLiteral("https://h.example.com/p#f?t=e5f61c9d2a7b40338fa1")));
        // No path: the host is the only honest label left, which is StoredUrl::title's own last fallback.
        CHECK(choose(QString(), QStringLiteral("https://h.example.com?t=e5f61c9d2a7b40338fa1"))
              == QStringLiteral("h.example.com"));
        NOLEAK(choose(QString(), QStringLiteral("https://h.example.com?t=e5f61c9d2a7b40338fa1")));
        // Every network scheme an IPTV or live source arrives on, not just http(s) — each carries the same
        // credential in the same place, which is the generalisation #200 had to make and this inherits.
        for (const QString& sc : QStringList{ "http", "https", "rtsp", "rtmp", "rtmps", "mms", "ftp", "srt" })
        {
            const QString u = sc + QStringLiteral("://live.example.com/ch1?token=sk-fake-000111");
            CHECK(choose(QString(), u) == QStringLiteral("ch1"));
            NOLEAK(choose(QString(), u));
        }
        // A launcher URI is NOT a network url: its query is a launch INSTRUCTION, not a credential. It is
        // also not a title, so it is named by its location — and, having no network scheme, by its base name.
        CHECK(!isUsable(QStringLiteral("com.epicgames.launcher://apps/Fortnite?action=launch&silent=true")));
    }

    // =====================================================================================================
    // 5. IDEMPOTENCE. A label that has been through the rule survives a second pass verbatim — the property
    //    that lets a caller apply it twice (a session builds titles_ with it, and the host applies it again
    //    at the point of display) without the second pass eroding the first pass's answer.
    // =====================================================================================================
    {
        const QStringList locations{
            kSigned,
            QStringLiteral("C:\\Users\\me\\My Videos\\a b.mkv"),
            QStringLiteral("\\\\server\\share\\Music\\My Song.flac"),
            QStringLiteral("file:///C:/Music/Song.mp3"),
            QStringLiteral("http://cdn.example.com/songs/Fly.mp3"),
            QStringLiteral("http://iptv.example.com/live/listener/hunter2/12345.ts"),
            QString(),
        };
        for (const QString& loc : locations)
        {
            const QString once = choose(QString(), loc);
            CHECK(choose(once, loc) == once);
            CHECK(choose(once, QString(), loc) == once);
            NOLEAK(once);
        }
        // …and a legitimate title is a fixed point of it too, which is the same property read the other way.
        for (const QString& t : QStringList{ "Who Framed Roger Rabbit?", "AC/DC — Back in Black", "Fly" })
            CHECK(choose(choose(t, kSigned), kSigned) == t);
    }

    // =====================================================================================================
    // 6. THE STANDING INVARIANT, stated once so it cannot be read off a list of individual cases: NO LABEL
    //    THIS RULE PRODUCES CONTAINS A '?' UNLESS IT CAME FROM A CANDIDATE THE RULE ACCEPTED AS A TITLE.
    //    A derived label is a host or a base name; neither can carry a query.
    // =====================================================================================================
    {
        const QStringList urls{
            kSigned,
            QStringLiteral("https://music.example.com/rest/download?id=1&u=listener&t=e5f61c9d2a7b40338fa1&s=9q4zt1"),
            QStringLiteral("http://iptv.example.com/live/listener/hunter2/12345.ts?token=sk-fake-000111"),
            QStringLiteral("https://dl.example.com/x/sk-fake-000111/file.mkv?Expires=99&Signature=9q4zt1"),
            QStringLiteral("rtmp://live.example.com/app?auth=e5f61c9d2a7b40338fa1"),
        };
        for (const QString& u : urls)
        {
            const QString derived = choose(QString(), u);
            CHECK(!derived.contains(QLatin1Char('?')));
            CHECK(!derived.contains(QLatin1Char('&')));
            CHECK(!derived.contains(QLatin1Char('=')));
            CHECK(!derived.contains(QStringLiteral("://")));
            CHECK(!derived.isEmpty());          // a url always leaves a host: silence is not the answer here
            NOLEAK(derived);
            // …and the same url offered as a CANDIDATE is refused, not cleaned.
            CHECK(!isUsable(u));
            NOLEAK(choose(u, u));
        }
    }

    if (failures == 0) std::printf("DISPLAYTITLE-OK\n");
    else               std::printf("DISPLAYTITLE-FAIL %d check(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
