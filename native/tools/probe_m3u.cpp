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

    if (fails == 0) printf("M3U-OK\n");
    return fails == 0 ? 0 : 1;
}
