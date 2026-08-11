// Headless check of the XMLTV EPG heart (#75, increment 3):
//
//   * xmltv::parseXmltvTime  — the "YYYYMMDDHHMMSS [+-]ZZZZ" -> UTC conversion, with and without the offset;
//   * xmltv::parseXmltv      — <channel>/<programme> extraction into the source-agnostic Programme model;
//   * xmltv::programmesForChannel — the tvg-id match (exact, case-folded) and the miss;
//   * xmltv::nowNext         — the containing programme + the following one, and empty when now is outside all;
//   * xmltv::gunzip          — round-trips a gzip buffer (compressed by python's gzip, an INDEPENDENT oracle,
//                              NOT by any code here) and passes plain bytes through untouched;
//   * StreamResolver::m3uHeaderTvgUrl — the #EXTM3U url-tvg / x-tvg-url header parse;
//   * browse::liveTvNowNextByTvgId / liveTvGuideCatalog — the display builders over the source-agnostic model.
//
// QtCore-only, offscreen-safe. Prints XMLTV-OK on success; any failure prints XMLTV-FAIL <cond> (line) + exits 1.
//
// FIXTURE INDEPENDENCE: every expected UTC datetime is CONSTRUCTED here by hand (QDate/QTime with Qt::UTC),
// never by running parseXmltvTime; the offset arithmetic (a "+0100" stamp is one hour EARLIER in UTC) is done
// on paper in the asserts. The gzip fixtures were produced by python's gzip module (see the comment on each);
// gunzip is measured against bytes it did not create.
#include "XmltvGuide.h"
#include "StreamResolver.h"   // m3uHeaderTvgUrl, M3uEntry
#include "LiveTvGuide.h"      // browse::liveTvNowNextByTvgId / liveTvGuideCatalog

#include <QCoreApplication>
#include <QDateTime>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "XMLTV-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

// UTC datetime built independently of the code under test.
static QDateTime utc(int y, int mo, int d, int h, int mi, int s = 0)
{ return QDateTime(QDate(y, mo, d), QTime(h, mi, s), Qt::UTC); }

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // ================= 1. parseXmltvTime — offset folding =========================================
    // The offset is applied so the RESULT is UTC: "+0100" wall-clock reads one hour earlier in UTC; "-0500"
    // reads five hours later; no offset is taken as already-UTC. Expected values computed by hand.
    CHECK(xmltv::parseXmltvTime(QStringLiteral("20240115143000 +0000")) == utc(2024, 1, 15, 14, 30));
    CHECK(xmltv::parseXmltvTime(QStringLiteral("20240115143000 +0100")) == utc(2024, 1, 15, 13, 30)); // -1h
    CHECK(xmltv::parseXmltvTime(QStringLiteral("20240115143000 -0500")) == utc(2024, 1, 15, 19, 30)); // +5h
    CHECK(xmltv::parseXmltvTime(QStringLiteral("20240115143000 +0530")) == utc(2024, 1, 15,  9,  0)); // -5h30
    CHECK(xmltv::parseXmltvTime(QStringLiteral("20240115143000"))       == utc(2024, 1, 15, 14, 30)); // no offset = UTC
    CHECK(!xmltv::parseXmltvTime(QStringLiteral("2024011514")).isValid());     // too short -> invalid
    CHECK(!xmltv::parseXmltvTime(QStringLiteral("2024AB15143000")).isValid()); // non-digit -> invalid
    CHECK(!xmltv::parseXmltvTime(QString()).isValid());                        // empty -> invalid

    // ================= 2. parseXmltv — channel + programme extraction =============================
    // Hand-authored XMLTV. bbc's programme is stamped +0100 so its UTC start (13:00) proves the parser feeds
    // the timestamp through the offset conversion, not just verbatim digits.
    static const char* kXml =
        "<tv>"
        "  <channel id=\"cnn.us\"><display-name>CNN</display-name></channel>"
        "  <channel id=\"bbc.uk\"><display-name>BBC One</display-name></channel>"
        "  <programme channel=\"cnn.us\" start=\"20240115140000 +0000\" stop=\"20240115150000 +0000\">"
        "    <title>News Hour</title><desc>World news</desc></programme>"
        "  <programme channel=\"cnn.us\" start=\"20240115150000 +0000\" stop=\"20240115160000 +0000\">"
        "    <title>Talk Show</title></programme>"
        "  <programme channel=\"bbc.uk\" start=\"20240115140000 +0100\" stop=\"20240115150000 +0100\">"
        "    <title>Breakfast</title></programme>"
        "</tv>";
    const xmltv::Guide g = xmltv::parseXmltv(QByteArray(kXml));

    CHECK(g.channelNames.value(QStringLiteral("cnn.us")) == QStringLiteral("CNN"));
    CHECK(g.channelNames.value(QStringLiteral("bbc.uk")) == QStringLiteral("BBC One"));
    CHECK(g.programmes.size() == 3);
    if (g.programmes.size() == 3)
    {
        const xmltv::Programme& p0 = g.programmes[0];
        CHECK(p0.channelId == QStringLiteral("cnn.us"));
        CHECK(p0.title == QStringLiteral("News Hour"));
        CHECK(p0.desc  == QStringLiteral("World news"));
        CHECK(p0.startUtc == utc(2024, 1, 15, 14, 0));
        CHECK(p0.stopUtc  == utc(2024, 1, 15, 15, 0));
        CHECK(g.programmes[1].title == QStringLiteral("Talk Show"));
        CHECK(g.programmes[1].desc.isEmpty());                                   // a <programme> with no <desc>
        // The +0100 programme: 14:00 wall-clock in a +1h zone == 13:00 UTC (the offset was applied on parse).
        CHECK(g.programmes[2].channelId == QStringLiteral("bbc.uk"));
        CHECK(g.programmes[2].startUtc == utc(2024, 1, 15, 13, 0));
    }

    // Malformed input never throws and keeps whatever parsed before the break.
    const xmltv::Guide bad = xmltv::parseXmltv(QByteArray(
        "<tv><programme channel=\"x\" start=\"20240115140000\" stop=\"20240115150000\">"
        "<title>Half</title></programme><programme channel=\"y\" start=\"20"));   // truncated mid-document
    CHECK(bad.programmes.size() >= 1);
    CHECK(bad.programmes[0].title == QStringLiteral("Half"));

    // ================= 3. programmesForChannel — the tvg-id match =================================
    CHECK(xmltv::programmesForChannel(g, QStringLiteral("cnn.us")).size() == 2);
    CHECK(xmltv::programmesForChannel(g, QStringLiteral("CNN.US")).size() == 2);  // ASCII case folded
    CHECK(xmltv::programmesForChannel(g, QStringLiteral("bbc.uk")).size() == 1);
    CHECK(xmltv::programmesForChannel(g, QStringLiteral("nope")).isEmpty());      // non-matching id -> none
    CHECK(xmltv::programmesForChannel(g, QString()).isEmpty());                   // empty id -> none

    // ================= 4. nowNext — current window + the following programme ======================
    const QVector<xmltv::Programme> cnn = xmltv::programmesForChannel(g, QStringLiteral("cnn.us"));
    {
        // now = 14:30 UTC: inside News Hour [14:00,15:00); next is Talk Show at 15:00.
        const xmltv::NowNext nn = xmltv::nowNext(cnn, utc(2024, 1, 15, 14, 30));
        CHECK(nn.hasCurrent && nn.current.title == QStringLiteral("News Hour"));
        CHECK(nn.hasNext    && nn.next.title    == QStringLiteral("Talk Show"));
    }
    {
        // now = 13:00 UTC: before everything -> no current, but News Hour is ahead.
        const xmltv::NowNext nn = xmltv::nowNext(cnn, utc(2024, 1, 15, 13, 0));
        CHECK(!nn.hasCurrent);
        CHECK(nn.hasNext && nn.next.title == QStringLiteral("News Hour"));
    }
    {
        // now = 16:30 UTC: after everything -> neither.
        const xmltv::NowNext nn = xmltv::nowNext(cnn, utc(2024, 1, 15, 16, 30));
        CHECK(!nn.hasCurrent && !nn.hasNext);
    }
    {
        // now = 15:00 UTC exactly: the [start,stop) rule makes Talk Show (15:00-16:00) current, News Hour past;
        // nothing starts strictly after 15:00, so there is no next. Kills a `<=`/`<` slip in either bound.
        const xmltv::NowNext nn = xmltv::nowNext(cnn, utc(2024, 1, 15, 15, 0));
        CHECK(nn.hasCurrent && nn.current.title == QStringLiteral("Talk Show"));
        CHECK(!nn.hasNext);
    }
    CHECK(!xmltv::nowNext({}, utc(2024, 1, 15, 14, 30)).hasCurrent);  // empty input -> empty result

    // ================= 5. gunzip — round-trip against a python-built buffer =======================
    // The uncompressed target and the two compressed buffers below were produced by python's gzip module:
    //   data = b'<tv><channel id="c1"><display-name>One</display-name></channel></tv>'   (68 bytes)
    //   (a) gzip.compress(data, mtime=0)                                — no FNAME
    //   (b) gzip.GzipFile(filename='epg.xml', mtime=0).write(data)      — FLG.FNAME set, "epg.xml\0" in header
    // gunzip must reproduce `data` from either, exercising the plain and the FNAME-skip header paths.
    static const char kData[] =
        "<tv><channel id=\"c1\"><display-name>One</display-name></channel></tv>";
    const QByteArray data(kData, int(sizeof(kData) - 1));
    CHECK(data.size() == 68);

    static const unsigned char kGzPlain[] = {
        0x1f,0x8b,0x08,0x00,0x00,0x00,0x00,0x00,0x02,0xff,0xb3,0x29,0x29,0xb3,0xb3,0x49,
        0xce,0x48,0xcc,0xcb,0x4b,0xcd,0x51,0xc8,0x4c,0xb1,0x55,0x4a,0x36,0x54,0xb2,0xb3,
        0x49,0xc9,0x2c,0x2e,0xc8,0x49,0xac,0xd4,0xcd,0x4b,0xcc,0x4d,0xb5,0xf3,0xcf,0x4b,
        0xb5,0xd1,0x47,0x11,0xb1,0xd1,0x87,0xea,0x00,0xb2,0x4a,0xca,0xec,0x00,0xd0,0xba,
        0x83,0x2e,0x44,0x00,0x00,0x00 };
    static const unsigned char kGzFname[] = {
        0x1f,0x8b,0x08,0x08,0x00,0x00,0x00,0x00,0x02,0xff,0x65,0x70,0x67,0x2e,0x78,0x6d,
        0x6c,0x00,0xb3,0x29,0x29,0xb3,0xb3,0x49,0xce,0x48,0xcc,0xcb,0x4b,0xcd,0x51,0xc8,
        0x4c,0xb1,0x55,0x4a,0x36,0x54,0xb2,0xb3,0x49,0xc9,0x2c,0x2e,0xc8,0x49,0xac,0xd4,
        0xcd,0x4b,0xcc,0x4d,0xb5,0xf3,0xcf,0x4b,0xb5,0xd1,0x47,0x11,0xb1,0xd1,0x87,0xea,
        0x00,0xb2,0x4a,0xca,0xec,0x00,0xd0,0xba,0x83,0x2e,0x44,0x00,0x00,0x00 };
    const QByteArray gzPlain(reinterpret_cast<const char*>(kGzPlain), int(sizeof(kGzPlain)));
    const QByteArray gzFname(reinterpret_cast<const char*>(kGzFname), int(sizeof(kGzFname)));
    CHECK(xmltv::gunzip(gzPlain) == data);   // plain gzip inflates to the original
    CHECK(xmltv::gunzip(gzFname) == data);   // FNAME header skipped, same result
    CHECK(xmltv::gunzip(data) == data);      // not gzip -> returned unchanged (a plain .xml feed)
    CHECK(xmltv::gunzip(QByteArray()) == QByteArray());  // empty -> empty, no crash

    // A gunzipped feed parses (end-to-end: the two heads meet).
    CHECK(xmltv::parseXmltv(xmltv::gunzip(gzPlain)).channelNames.value(QStringLiteral("c1"))
          == QStringLiteral("One"));

    // ================= 6. m3uHeaderTvgUrl — the #EXTM3U header EPG url =============================
    CHECK(StreamResolver::m3uHeaderTvgUrl(
              QStringLiteral("#EXTM3U url-tvg=\"http://h/epg.xml.gz\"\n#EXTINF:-1,A\nhttp://h/a\n"))
          == QStringLiteral("http://h/epg.xml.gz"));
    CHECK(StreamResolver::m3uHeaderTvgUrl(QStringLiteral("#EXTM3U x-tvg-url=\"http://h/e2.xml\"\n"))
          == QStringLiteral("http://h/e2.xml"));                    // the older synonym
    CHECK(StreamResolver::m3uHeaderTvgUrl(
              QStringLiteral("#EXTM3U x-tvg-url=\"http://h/old.xml\" url-tvg=\"http://h/new.xml\"\n"))
          == QStringLiteral("http://h/new.xml"));                   // url-tvg wins when both are present
    CHECK(StreamResolver::m3uHeaderTvgUrl(QStringLiteral("#EXTM3U\n#EXTINF:-1,A\nhttp://h/a\n")).isEmpty());
    CHECK(StreamResolver::m3uHeaderTvgUrl(QStringLiteral("#EXTINF:-1,A\nhttp://h/a\n")).isEmpty()); // no header line

    // ================= 7. display builders over the source-agnostic model =========================
    // Two channels: cnn (tvg-id matches the guide) and a local one with NO tvg-id.
    QVector<M3uEntry> chans;
    M3uEntry c; c.title = QStringLiteral("CNN"); c.url = QStringLiteral("http://x/cnn.ts"); c.tvgId = QStringLiteral("cnn.us");
    M3uEntry l; l.title = QStringLiteral("Local9"); l.url = QStringLiteral("http://x/l9.ts"); // tvgId empty
    chans << c << l;

    {
        // now = 14:30 UTC: cnn is on News Hour, next Talk Show. The local channel is absent (no tvg-id match).
        const QHash<QString, QString> nn = browse::liveTvNowNextByTvgId(chans, g, utc(2024, 1, 15, 14, 30));
        CHECK(nn.value(QStringLiteral("cnn.us"))
              == QStringLiteral("Now: News Hour · Next: Talk Show"));   // "Now … · Next …"
        CHECK(nn.size() == 1);                                               // Local9 (no tvg-id) not present
    }
    {
        // A now-only case (after the last programme's start but a bbc channel that has just its single show):
        // build a one-programme guide and assert "Now:" with no "· Next:".
        const QHash<QString, QString> nn = browse::liveTvNowNextByTvgId(chans, g, utc(2024, 1, 15, 15, 30));
        CHECK(nn.value(QStringLiteral("cnn.us")) == QStringLiteral("Now: Talk Show")); // nothing after -> Now only
    }

    // The guide grid for the day 2024-01-15 UTC. Expect: [hdr CNN] then its 2 programmes (14:00 marked ● when
    // now is inside it), then [hdr Local9] with no programmes (still shown).
    {
        const QDateTime dayStart = utc(2024, 1, 15, 0, 0);
        const QDateTime dayEnd   = utc(2024, 1, 16, 0, 0);
        const MediaCatalog grid = browse::liveTvGuideCatalog(QStringLiteral("Prov"), chans, g,
                                                             utc(2024, 1, 15, 14, 30), dayStart, dayEnd);
        // rows: hdr(CNN) prog prog hdr(Local9)  -> 4 rows
        CHECK(grid.items.size() == 4);
        if (grid.items.size() == 4)
        {
            CHECK(grid.items[0].type == QStringLiteral("_livetvheader"));
            CHECK(grid.items[0].title == QStringLiteral("CNN"));             // EPG display-name preferred
            CHECK(grid.items[1].type == QStringLiteral("_guideprog"));
            CHECK(grid.items[1].title.startsWith(QStringLiteral("●")));  // ● on the on-air programme
            CHECK(grid.items[1].title.contains(QStringLiteral("News Hour")));
            CHECK(grid.items[2].type == QStringLiteral("_guideprog"));
            CHECK(!grid.items[2].title.startsWith(QStringLiteral("●"))); // Talk Show not on air -> no ●
            CHECK(grid.items[3].type == QStringLiteral("_livetvheader"));
            CHECK(grid.items[3].title == QStringLiteral("Local9"));          // no tvg-id -> playlist title, still shown
        }
        // Programme rows are non-playable (no url), like the header.
        CHECK(grid.items[1].url.isEmpty());
    }

    if (failures == 0) { std::puts("XMLTV-OK"); return 0; }
    std::fprintf(stderr, "XMLTV: %d check(s) failed\n", failures);
    return 1;
}
