// Headless test for StreamResolver's playlist classification: HLS manifest vs IPTV list vs
// PlayStation disc set, plus relative-URL resolution. Prints M3U-OK when every assert holds.
#include <QCoreApplication>
#include "../src/media/StreamResolver.h"

static int fails = 0;
#define CHECK(cond, name) do { if (cond) printf("PASS %s\n", name); \
    else { printf("FAIL %s\n", name); ++fails; } } while (0)

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    CHECK(StreamResolver::isM3uRef("http://x/list.m3u8?token=1"), "isM3uRef ignores the query");
    CHECK(!StreamResolver::isM3uRef("http://x/video.mp4"), "isM3uRef rejects plain media");

    CHECK(StreamResolver::isHlsManifest("#EXTM3U\n#EXT-X-TARGETDURATION:10\nseg1.ts"),
          "HLS manifest detected by #EXT-X-");
    CHECK(!StreamResolver::isHlsManifest("#EXTM3U\n#EXTINF:-1,Ch1\nhttp://a/1"),
          "plain media list is not HLS");

    const auto iptv = StreamResolver::parseM3u(
        "#EXTM3U\n#EXTINF:-1,Channel One\nhttp://srv/one\n#EXTINF:-1,Channel Two\nrel/two.ts\n",
        "http://host/pl/list.m3u");
    CHECK(iptv.size() == 2, "parseM3u finds both entries");
    CHECK(iptv[0].title == "Channel One" && iptv[0].url == "http://srv/one", "absolute entry kept");
    CHECK(iptv[1].url == "http://host/pl/rel/two.ts", "relative entry resolved against the playlist URL");
    CHECK(!StreamResolver::looksLikeDiscPlaylist(iptv), "IPTV list is not a disc set");

    const auto discs = StreamResolver::parseM3u(
        "Game (Disc 1).chd\nGame (Disc 2).chd\n", "C:/roms/psx/Game.m3u");
    CHECK(discs.size() == 2, "disc list parses");
    CHECK(StreamResolver::looksLikeDiscPlaylist(discs), "all-disc entries detected as a disc set");

    // ---- an IPTV list's entries and the playlist's own headers (#59) ------------------------------------
    // A gated playlist is fetched with the source's proxyHeaders; its ENTRIES are separate URLs on whatever
    // hosts the list names. The provider's own channels sit on the provider's origin and are gated the same
    // way; a third-party link in the same list is a different source and must leave with nothing.
    {
        const auto mixed = StreamResolver::parseM3u(
            "#EXTM3U\n"
            "#EXTINF:-1,Own channel\nhttps://iptv.test/live/1.ts\n"      // same origin as the playlist
            "#EXTINF:-1,Own channel on another port\nhttps://iptv.test:8443/live/2.ts\n"
            "#EXTINF:-1,Somebody else's\nhttps://other.test/live/3.ts\n"
            "#EXTINF:-1,Relative\nrel/4.ts\n",                            // resolves onto the playlist's host
            "https://iptv.test/pl/list.m3u");
        CHECK(mixed.size() == 4, "the mixed list parses");

        StreamHeaders::Headers listHeaders;
        listHeaders.insert("Referer", "https://iptv.test/portal");
        listHeaders.insert("X-Token", "PROBE-TOKEN");
        const auto per = StreamResolver::entryHeaders(mixed, "https://iptv.test/pl/list.m3u", listHeaders);

        CHECK(per.size() == mixed.size(), "one answer per entry, parallel to the list");
        CHECK(per.value(0) == listHeaders, "an entry on the playlist's own origin inherits its headers");
        // THE hygiene assertions. Each is a different way to not be the playlist's origin, and each has to
        // be here: with only the third-party case, a rule that compared HOSTS and ignored the port would
        // pass, and the app would hand a gated token to a different service on the same machine.
        CHECK(per.value(1).isEmpty(), "a different PORT on the same host is a different origin — nothing");
        CHECK(per.value(2).isEmpty(), "a third-party entry in the same list gets none of them");
        CHECK(per.value(3) == listHeaders, "a RELATIVE entry resolved onto the playlist's host inherits them");
        // Empty, not absent: an entry with no headers still has an entry, because that empty set is what
        // clears the previous channel's headers at the player.
        CHECK(per.size() == 4, "an entry entitled to nothing still occupies its slot in the list");

        // A playlist that was not gated hands nothing to anyone — including to entries on its own origin.
        const auto none = StreamResolver::entryHeaders(mixed, "https://iptv.test/pl/list.m3u", {});
        CHECK(none.size() == 4 && none.value(0).isEmpty() && none.value(3).isEmpty(),
              "an ungated playlist's entries get nothing, same-origin or not");
    }

    if (fails == 0) printf("M3U-OK\n");
    return fails == 0 ? 0 : 1;
}
