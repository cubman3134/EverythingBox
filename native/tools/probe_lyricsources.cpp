// Headless check of #142's OTHER TWO lyric sources and, above all, of the order the three of them are tried
// in (src/media/LyricSources.h, src/media/LyricFetch.cpp, and the lyric half of src/media/AudioTags.cpp).
// probe_lyrics already pins the LRC parser itself; nothing there knows a tag or a network exists.
//
// What it pins:
//   1. SYLT -> LRC rendering, through LrcLyrics::formatTimestamp / renderLrc: the timestamp spelling, the
//      rounding that must not overflow a hundredths field, and the round-trip back through parseLrc.
//   2. EMBEDDED LYRICS out of the one tag pass, from fixtures built byte by byte with tools/MusicFixtures.h:
//      an ID3v2 USLT sheet, a USLT frame with a DESCRIPTION (which TagLib files under "LYRICS:<DESC>", a key
//      a naive lookup misses entirely), an MP4 ©lyr atom, a Vorbis LYRICS comment, LRC text stored inside a
//      plain lyrics tag (very common, and it must come out SYNCED), a millisecond SYLT frame in both of the
//      two fragment conventions that exist in the wild, and the two SYLT frames the reader must REFUSE — an
//      MPEG-frame-counted one and a chord chart.
//   3. THE PRECEDENCE, in every combination: sidecar > embedded > LRCLIB, decided on EXISTENCE and not on
//      quality, so a plain sidecar still beats a synced network answer. Plus the tie-breaks inside a tier.
//   4. needsOnline: the politeness gate, true only when both LOCAL tiers are empty.
//   5. The LRCLIB protocol, both endpoints: the URL a set of tags produces (including the fields that are
//      deliberately LEFT OUT), a record, a 404 body, an instrumental, and a search array whose first entries
//      carry no words.
//   6. The CACHE: a fetched sheet round-trips out of the item's MetaCache folder with no network, a recorded
//      miss suppresses the next lookup, a success clears a standing miss, and the miss expires.
//
// Prints LYRICSOURCES-OK on success; any failure prints LYRICSOURCES-FAIL <cond> (line) and exits non-zero.
//
// Isolation: AppPaths::dataDir() is this process's own scratch dir (issue #42), so the fixtures and the
// MetaCache folders are written under it and the tree goes away at exit. Nothing is written beside the exe,
// and NOTHING HERE TOUCHES THE NETWORK — every online case is driven through the pure parsers with a body
// written out in the probe, which is why the suite stays offline.
#include "AppPaths.h"
#include "AudioTags.h"
#include "LrcLyrics.h"
#include "LyricFetch.h"
#include "LyricSources.h"
#include "MetaCache.h"
#include "MusicFixtures.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QString>
#include <cstdio>

static int g_fails = 0;

#define CHECK(cond)                                                                        \
    do {                                                                                   \
        if (!(cond)) { std::printf("LYRICSOURCES-FAIL %s (%d)\n", #cond, __LINE__); ++g_fails; } \
    } while (0)

namespace LS = LyricSources;

// A two-line synced sheet and a two-line plain one, reused across the precedence matrix. Each carries the
// name of the tier that supplied it in its own text, so an assertion about WHICH tier won can be made on the
// content rather than only on the Source enum — a resolve() that returned the right label with the wrong
// words would otherwise pass.
static QString syncedFrom(const QString& tier)
{
    return QStringLiteral("[00:01.00]%1 one\n[00:05.00]%1 two\n").arg(tier);
}
static QString plainFrom(const QString& tier)
{
    return QStringLiteral("%1 one\n%1 two\n").arg(tier);
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const QString root = AppPaths::dataDir() + QStringLiteral("/lyricfix");
    QDir().mkpath(root);

    // --- 1. SYLT's rendering half: seconds -> the LRC timestamp spelling ---------------------------------
    {
        CHECK(LrcLyrics::formatTimestamp(0.0) == QStringLiteral("00:00.00"));
        CHECK(LrcLyrics::formatTimestamp(1.5) == QStringLiteral("00:01.50"));
        CHECK(LrcLyrics::formatTimestamp(4.25) == QStringLiteral("00:04.25"));
        // Minutes are NOT wrapped at 60: a 74-minute live set's last line is legal LRC and parseTimestamp
        // reads it back.
        CHECK(LrcLyrics::formatTimestamp(74 * 60 + 12.3) == QStringLiteral("74:12.30"));
        // Rounded to hundredths as ONE number. Splitting first and rounding the fraction separately turns
        // 61.999 into "01:01.100" — a fraction that overflowed its own two-digit field.
        CHECK(LrcLyrics::formatTimestamp(61.999) == QStringLiteral("01:02.00"));
        // Negative clamps rather than rendering "[-00:03.00]", which nothing parses.
        CHECK(LrcLyrics::formatTimestamp(-3.0) == QStringLiteral("00:00.00"));

        QVector<LrcLyrics::LyricLine> lines{ { 1.5, QStringLiteral("first") }, { 4.25, QStringLiteral("second") } };
        const QString lrc = LrcLyrics::renderLrc(lines);
        CHECK(lrc == QStringLiteral("[00:01.50]first\n[00:04.25]second\n"));
        // The round trip is the point: SYLT becomes LRC so the ONE parser handles it.
        const LrcLyrics::Lyrics back = LrcLyrics::parseLrc(lrc);
        CHECK(back.synced);
        CHECK(back.lines.size() == 2);
        CHECK(back.lines[0].timeSec > 1.49 && back.lines[0].timeSec < 1.51);
        CHECK(back.lines[1].text == QStringLiteral("second"));
    }

    // --- 2. Embedded lyrics, out of the one tag pass ------------------------------------------------------
    {
        // 2a. A plain USLT sheet with no description: TagLib folds it onto the "LYRICS" property key.
        const QString uslt = QStringLiteral("Line one\nLine two\n\nLine three");
        const QString p2a  = root + QStringLiteral("/uslt.mp3");
        CHECK(writeFixture(p2a, mp3File(id3TextFrame("TIT2", QStringLiteral("Uslt Song"))
                                        + id3TextFrame("TPE1", QStringLiteral("Uslt Band"))
                                        + id3UsltFrame(QString(), uslt))));
        const AudioTags::Tags t2a = AudioTags::read(p2a);
        CHECK(t2a.title == QStringLiteral("Uslt Song"));
        CHECK(t2a.lyrics == uslt);          // interior blank line preserved: it is the sheet's verse break
        CHECK(t2a.syncedLyrics.isEmpty());  // no SYLT frame in the file
        // Unsynced degrades rather than hiding: it still parses to lines, just untimed.
        const LrcLyrics::Lyrics l2a = LrcLyrics::parseLrc(t2a.lyrics);
        CHECK(!l2a.synced);
        CHECK(l2a.lines.size() == 3);

        // 2b. A USLT frame WITH a description. TagLib keeps the description in the key ("LYRICS:CHORUS") so
        // two frames in different languages cannot collide — and a reader that only ever looked up "LYRICS"
        // would report this file as having no lyrics at all.
        const QString p2b = root + QStringLiteral("/usltdesc.mp3");
        CHECK(writeFixture(p2b, mp3File(id3TextFrame("TIT2", QStringLiteral("Described"))
                                        + id3UsltFrame(QStringLiteral("Chorus"), QStringLiteral("Described words")))));
        CHECK(AudioTags::read(p2b).lyrics == QStringLiteral("Described words"));

        // 2c. LRC TEXT INSIDE A PLAIN LYRICS TAG. Every tagger that writes lyrics from an .lrc does this, so
        // the "unsynced tag" is very often synced — which is exactly why AudioTags does not decide, and
        // parseLrc does.
        const QString p2c = root + QStringLiteral("/usltlrc.mp3");
        CHECK(writeFixture(p2c, mp3File(id3TextFrame("TIT2", QStringLiteral("Timed In A Text Tag"))
                                        + id3UsltFrame(QString(), syncedFrom(QStringLiteral("tag"))))));
        const LrcLyrics::Lyrics l2c = LrcLyrics::parseLrc(AudioTags::read(p2c).lyrics);
        CHECK(l2c.synced);
        CHECK(l2c.lines.size() == 2);
        CHECK(l2c.lines[0].text == QStringLiteral("tag one"));

        // 2d. A millisecond SYLT frame, whole-line convention (no fragment announces itself with a newline),
        // so every fragment is its own line.
        const QString p2d = root + QStringLiteral("/sylt.mp3");
        QList<QPair<quint32, QString>> sylt{ { 1500u, QStringLiteral("Synced line one") },
                                             { 4250u, QStringLiteral("Synced line two") } };
        CHECK(writeFixture(p2d, mp3File(id3TextFrame("TIT2", QStringLiteral("Sylt Song")) + id3SyltFrame(sylt))));
        const AudioTags::Tags t2d = AudioTags::read(p2d);
        CHECK(t2d.syncedLyrics == QStringLiteral("[00:01.50]Synced line one\n[00:04.25]Synced line two\n"));
        const LrcLyrics::Lyrics l2d = LrcLyrics::parseLrc(t2d.syncedLyrics);
        CHECK(l2d.synced);
        CHECK(l2d.lines.size() == 2);

        // 2e. The OTHER convention: the ID3v2 spec's, where a fragment beginning with a newline starts a line
        // and the rest continue it. A word-synced frame therefore renders LINE-level, at the line's own start
        // time — the same call parseLrc makes about enhanced <mm:ss.xx> word tags.
        const QString p2e = root + QStringLiteral("/syltword.mp3");
        QList<QPair<quint32, QString>> words{ { 1000u, QStringLiteral("Never") },
                                              { 1200u, QStringLiteral(" gonna") },
                                              { 1400u, QStringLiteral(" give") },
                                              { 3000u, QStringLiteral("\nNever") },
                                              { 3200u, QStringLiteral(" gonna") } };
        CHECK(writeFixture(p2e, mp3File(id3TextFrame("TIT2", QStringLiteral("Worded")) + id3SyltFrame(words))));
        const AudioTags::Tags t2e = AudioTags::read(p2e);
        CHECK(t2e.syncedLyrics == QStringLiteral("[00:01.00]Never gonna give\n[00:03.00]Never gonna\n"));

        // 2f. REFUSED: an MPEG-FRAME-counted SYLT. Converting it needs the frame rate, and inventing one puts
        // every line at the wrong moment — the most visible way a karaoke scroll can be wrong. Nothing is a
        // better answer than something mistimed, and the tier below is then free to answer instead.
        const QString p2f = root + QStringLiteral("/syltframes.mp3");
        CHECK(writeFixture(p2f, mp3File(id3TextFrame("TIT2", QStringLiteral("Frames"))
                                        + id3SyltFrame(sylt, /*timestampFormat*/ 0x01))));
        CHECK(AudioTags::read(p2f).syncedLyrics.isEmpty());

        // 2g. REFUSED: a SYLT carrying a CHORD CHART (content type 5). It is a well-formed frame that is not
        // a lyric sheet, and rendering it as one would be worse than showing nothing.
        const QString p2g = root + QStringLiteral("/syltchords.mp3");
        CHECK(writeFixture(p2g, mp3File(id3TextFrame("TIT2", QStringLiteral("Chords"))
                                        + id3SyltFrame(sylt, 0x02, /*contentType*/ 0x05))));
        CHECK(AudioTags::read(p2g).syncedLyrics.isEmpty());

        // 2h. MP4's ©lyr atom — the container the issue names by name for the embedded source.
        const QString p2h = root + QStringLiteral("/lyr.m4a");
        CHECK(writeFixture(p2h, m4aFile(mp4TextItem(itunesName("nam"), QStringLiteral("Atom Song"))
                                        + mp4TextItem(itunesName("lyr"), QStringLiteral("Atom words here")))));
        const AudioTags::Tags t2h = AudioTags::read(p2h);
        CHECK(t2h.title == QStringLiteral("Atom Song"));
        CHECK(t2h.lyrics == QStringLiteral("Atom words here"));

        // 2i. A Vorbis LYRICS comment (.flac) — the third container spelling, folded onto the same key.
        const QString p2i = root + QStringLiteral("/vorbis.flac");
        QByteArray flac("fLaC", 4);
        flac.append(flacBlock(0, flacStreamInfo(44100, 2, 16, 132300), false));
        flac.append(flacBlock(4, flacVorbisComment({ QByteArray("TITLE=Vorbis Song"),
                                                     QByteArray("LYRICS=Vorbis words here") }), true));
        CHECK(writeFixture(p2i, flac));
        CHECK(AudioTags::read(p2i).lyrics == QStringLiteral("Vorbis words here"));

        // 2j. A file with no lyric tag of any kind reports neither field — and is still not "empty", because
        // it has a title. Lyrics deliberately do not participate in isEmpty(): a lyric sheet is not something
        // a library can file a track under.
        const QString p2j = root + QStringLiteral("/nolyrics.mp3");
        CHECK(writeFixture(p2j, mp3File(id3TextFrame("TIT2", QStringLiteral("Wordless")))));
        const AudioTags::Tags t2j = AudioTags::read(p2j);
        CHECK(t2j.lyrics.isEmpty());
        CHECK(t2j.syncedLyrics.isEmpty());
        CHECK(!t2j.isEmpty());
        // ...and the converse: a file whose ONLY tag is a lyric sheet is still isEmpty(), because there is
        // nothing to file it under. A tripwire on a deliberate absence of behaviour, per CONTRIBUTING.
        const QString p2k = root + QStringLiteral("/onlylyrics.mp3");
        CHECK(writeFixture(p2k, mp3File(id3UsltFrame(QString(), QStringLiteral("just words")))));
        const AudioTags::Tags t2k = AudioTags::read(p2k);
        CHECK(t2k.lyrics == QStringLiteral("just words"));
        CHECK(t2k.isEmpty());
    }

    // --- 3. THE PRECEDENCE: sidecar > embedded > LRCLIB, in every combination ------------------------------
    {
        const QString S = syncedFrom(QStringLiteral("sidecar"));
        const QString E = syncedFrom(QStringLiteral("embedded"));
        const QString L = syncedFrom(QStringLiteral("lrclib"));

        struct Case { bool s, e, l; LS::Source want; const char* words; };
        const Case cases[] = {
            { false, false, false, LS::Source::None,     nullptr },
            { false, false, true,  LS::Source::Lrclib,   "lrclib one" },
            { false, true,  false, LS::Source::Embedded, "embedded one" },
            { false, true,  true,  LS::Source::Embedded, "embedded one" },
            { true,  false, false, LS::Source::Sidecar,  "sidecar one" },
            { true,  false, true,  LS::Source::Sidecar,  "sidecar one" },
            { true,  true,  false, LS::Source::Sidecar,  "sidecar one" },
            { true,  true,  true,  LS::Source::Sidecar,  "sidecar one" },
        };
        for (const Case& c : cases)
        {
            LS::Candidates cand;
            if (c.s) cand.sidecar = S;
            if (c.e) cand.embeddedSynced = E;
            if (c.l) cand.lrclib = L;
            const LS::Choice got = LS::resolve(cand);
            CHECK(got.source == c.want);
            if (c.words)
            {
                CHECK(!got.lyrics.lines.isEmpty());
                if (!got.lyrics.lines.isEmpty())
                    CHECK(got.lyrics.lines[0].text == QString::fromLatin1(c.words));
            }
            else
            {
                CHECK(got.lyrics.lines.isEmpty());
            }
            // The politeness gate is a function of the LOCAL tiers only, never of tier 3.
            CHECK(LS::needsOnline(cand) == (!c.s && !c.e));
        }

        // A PLAIN sidecar still beats a SYNCED network answer. This is the assertion that says the precedence
        // is a list and not a score: the user put that file there to override what the internet says, and a
        // rule that preferred the timed one would hand their correction straight back.
        LS::Candidates over;
        over.sidecar = plainFrom(QStringLiteral("sidecar"));
        over.lrclib  = L;
        const LS::Choice ovr = LS::resolve(over);
        CHECK(ovr.source == LS::Source::Sidecar);
        CHECK(!ovr.lyrics.synced);                     // and it degrades to a plain sheet, it does not hide
        CHECK(ovr.lyrics.lines.size() == 2);

        // A plain EMBEDDED tag likewise beats a synced LRCLIB answer, one tier down.
        LS::Candidates emb;
        emb.embeddedPlain = plainFrom(QStringLiteral("embedded"));
        emb.lrclib        = L;
        CHECK(LS::resolve(emb).source == LS::Source::Embedded);
        CHECK(!LS::needsOnline(emb));

        // INSIDE tier 2, where there is no user intent to respect, quality DOES decide: SYLT beats USLT.
        LS::Candidates both;
        both.embeddedSynced = E;
        both.embeddedPlain  = plainFrom(QStringLiteral("uslt"));
        const LS::Choice tie = LS::resolve(both);
        CHECK(tie.source == LS::Source::Embedded);
        CHECK(tie.lyrics.synced);
        CHECK(tie.lyrics.lines[0].text == QStringLiteral("embedded one"));

        // A tier that is present but has no WORDS does not shadow the tier below it: an empty USLT frame, or
        // a sidecar of nothing but blank lines, is not an answer.
        LS::Candidates blank;
        blank.sidecar        = QStringLiteral("   \n\n \n");
        blank.embeddedSynced = QStringLiteral("");
        blank.embeddedPlain  = QStringLiteral("\n");
        blank.lrclib         = L;
        CHECK(LS::resolve(blank).source == LS::Source::Lrclib);
        CHECK(LS::needsOnline(blank));

        // ...and an LRC file with ID TAGS BUT NO LINES ([ti:]/[ar:] and nothing else) is the same story: it
        // parses, it names a title, it has no words, so it must not shadow the tier below.
        LS::Candidates idonly;
        idonly.sidecar = QStringLiteral("[ti:Title Only]\n[ar:Nobody]\n");
        idonly.lrclib  = L;
        CHECK(LS::resolve(idonly).source == LS::Source::Lrclib);

        CHECK(LS::sourceId(LS::Source::Sidecar) == QStringLiteral("sidecar"));
        CHECK(LS::sourceId(LS::Source::Embedded) == QStringLiteral("embedded"));
        CHECK(LS::sourceId(LS::Source::Lrclib) == QStringLiteral("lrclib"));
        CHECK(LS::sourceId(LS::Source::None) == QStringLiteral("none"));
    }

    // --- 4. The LRCLIB protocol, both endpoints, with no socket -------------------------------------------
    {
        Lrclib::Query q;
        CHECK(!Lrclib::isUsable(q));                                   // nothing to ask about
        q.title = QStringLiteral("Song");
        CHECK(!Lrclib::isUsable(q));                                   // a title alone matches half the world
        q.artist = QStringLiteral("Band");
        CHECK(Lrclib::isUsable(q));
        q.artist = QStringLiteral("   ");
        CHECK(!Lrclib::isUsable(q));                                   // whitespace is not an artist
        q.artist = QStringLiteral("The Band");
        q.album  = QStringLiteral("The Album");
        q.durationSec = 217;

        // FullyEncoded, because that is what actually goes on the wire: the default toString() renders the
        // query back in its DECODED form, so asserting on that would leave a spaced artist's encoding untested.
        const QString get = Lrclib::getUrl(q).toString(QUrl::FullyEncoded);
        CHECK(get.startsWith(QStringLiteral("https://lrclib.net/api/get?")));
        CHECK(get.contains(QStringLiteral("artist_name=The%20Band")));
        CHECK(get.contains(QStringLiteral("track_name=Song")));
        CHECK(get.contains(QStringLiteral("album_name=The%20Album")));
        CHECK(get.contains(QStringLiteral("duration=217")));

        // An UNKNOWN duration is omitted, not sent as 0 — /api/get matches the duration, and 0 matches nothing
        // at all, so sending it would turn every untimed container into a guaranteed miss.
        Lrclib::Query nodur = q;
        nodur.durationSec = 0;
        nodur.album.clear();
        const QString nd = Lrclib::getUrl(nodur).toString(QUrl::FullyEncoded);
        CHECK(!nd.contains(QStringLiteral("duration=")));
        CHECK(!nd.contains(QStringLiteral("album_name=")));

        // The fuzzy fallback deliberately drops album AND duration: they are the fields whose disagreement
        // made /api/get miss in the first place, so repeating them would reproduce the miss.
        const QString search = Lrclib::searchUrl(q).toString(QUrl::FullyEncoded);
        CHECK(search.startsWith(QStringLiteral("https://lrclib.net/api/search?")));
        CHECK(search.contains(QStringLiteral("artist_name=The%20Band")));
        CHECK(!search.contains(QStringLiteral("album_name=")));
        CHECK(!search.contains(QStringLiteral("duration=")));

        // A record with both fields: synced wins inside the tier.
        const Lrclib::Response r = Lrclib::parseGet(
            QByteArray("{\"id\":42,\"trackName\":\"Song\",\"instrumental\":false,"
                       "\"plainLyrics\":\"plain one\\nplain two\","
                       "\"syncedLyrics\":\"[00:01.00]timed one\\n[00:05.00]timed two\"}"));
        CHECK(r.valid);
        CHECK(Lrclib::bestText(r).startsWith(QStringLiteral("[00:01.00]timed one")));

        // Plain only: still an answer, and it lands as an UNSYNCED sheet rather than as nothing.
        const Lrclib::Response rp = Lrclib::parseGet(
            QByteArray("{\"id\":7,\"instrumental\":false,\"plainLyrics\":\"only plain\",\"syncedLyrics\":null}"));
        CHECK(rp.valid);
        CHECK(Lrclib::bestText(rp) == QStringLiteral("only plain"));
        CHECK(!LrcLyrics::parseLrc(Lrclib::bestText(rp)).synced);

        // An INSTRUMENTAL is a positive "there are no words", not a failure — and the FLAG is what says so,
        // not the emptiness of the two text fields. Contributors do upload instrumental records carrying a
        // placeholder line ("♪", "[Instrumental]"), and a panel that scrolls one of those past a listener is
        // worse than the no panel they should get, so the flag overrides text that is present.
        const Lrclib::Response ri = Lrclib::parseGet(
            QByteArray("{\"id\":9,\"instrumental\":true,\"plainLyrics\":\"[Instrumental]\","
                       "\"syncedLyrics\":\"[00:00.00] \\u266a\"}"));
        CHECK(ri.valid);
        CHECK(ri.instrumental);
        CHECK(Lrclib::bestText(ri).isEmpty());

        // A 404 body PARSES as JSON and must not be mistaken for a record.
        const Lrclib::Response r404 = Lrclib::parseGet(
            QByteArray("{\"code\":404,\"name\":\"TrackNotFound\",\"message\":\"Failed to find specified track\"}"));
        CHECK(!r404.valid);
        CHECK(Lrclib::bestText(r404).isEmpty());
        CHECK(!Lrclib::parseGet(QByteArray("<html>502</html>")).valid);   // and neither is a proxy error page
        CHECK(!Lrclib::parseGet(QByteArray()).valid);

        // /api/search returns an array. Entries with no words are SKIPPED rather than accepted: the index
        // carries records nobody has contributed lyrics for, and taking the first of those would turn a
        // recoverable miss into a settled one.
        const Lrclib::Response rs = Lrclib::parseSearch(
            QByteArray("[{\"id\":1,\"plainLyrics\":null,\"syncedLyrics\":null},"
                       "{\"id\":2,\"instrumental\":true},"
                       "{\"id\":3,\"plainLyrics\":\"third has words\"}]"));
        CHECK(rs.valid);
        CHECK(Lrclib::bestText(rs) == QStringLiteral("third has words"));
        CHECK(!Lrclib::parseSearch(QByteArray("[]")).valid);
        CHECK(!Lrclib::parseSearch(QByteArray("{\"id\":1}")).valid);      // an object where an array was due
    }

    // --- 5. The cache: fetched once, then offline ---------------------------------------------------------
    {
        const QString audio = root + QStringLiteral("/sylt.mp3");   // a real path, from section 2
        const QString key   = LyricFetch::cacheKey(audio);
        CHECK(!key.isEmpty());
        CHECK(LyricFetch::cacheKey(QString()).isEmpty());
        // The key is the ABSOLUTE path, so the same track reached through a "…/x/../x/…" spelling is one
        // cache entry rather than two folders and two fetches.
        const QString detour = root + QStringLiteral("/../lyricfix/sylt.mp3");
        CHECK(LyricFetch::cacheKey(detour) == key);

        CHECK(LyricFetch::cachedText(key).isEmpty());               // nothing cached yet
        CHECK(!LyricFetch::missRecorded(key));

        const QString fetched = syncedFrom(QStringLiteral("lrclib"));
        LyricFetch::storeText(key, fetched);
        CHECK(LyricFetch::cachedText(key) == fetched);              // round-trips, with no network
        CHECK(!LyricFetch::missRecorded(key));
        // ...and it is the one MetaCache folder, not a private store beside it.
        CHECK(QDir(MetaCache::dirFor(key)).exists());

        // An empty answer is never written: it would read back as a cached sheet with no words and suppress
        // the miss that should have been recorded instead.
        const QString key2 = LyricFetch::cacheKey(root + QStringLiteral("/uslt.mp3"));
        LyricFetch::storeText(key2, QStringLiteral("   \n "));
        CHECK(LyricFetch::cachedText(key2).isEmpty());

        // A miss suppresses the next lookup...
        LyricFetch::storeMiss(key2);
        CHECK(LyricFetch::missRecorded(key2));
        CHECK(LyricFetch::cachedText(key2).isEmpty());
        // ...and a later success CLEARS it, because the record is replaced as a unit rather than merged into.
        LyricFetch::storeText(key2, fetched);
        CHECK(LyricFetch::cachedText(key2) == fetched);
        CHECK(!LyricFetch::missRecorded(key2));

        // The expiry rule, without waiting a month for it. A miss is honoured for missRetryDays and then
        // lapses, so a track whose lyrics somebody contributes later still reaches the user.
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        const qint64 day = 24 * 60 * 60;
        CHECK(!LyricFetch::missIsFresh(0, now));                                  // no miss recorded
        CHECK(LyricFetch::missIsFresh(now, now));
        CHECK(LyricFetch::missIsFresh(now - day, now));
        CHECK(LyricFetch::missIsFresh(now - (LyricFetch::missRetryDays() - 1) * day, now));
        CHECK(!LyricFetch::missIsFresh(now - (LyricFetch::missRetryDays() + 1) * day, now));
        // A missAt in the FUTURE means the clock moved backwards (an NTP correction, a restored backup). It
        // is honoured rather than treated as expired, or such a machine re-asks about every wordless track.
        CHECK(LyricFetch::missIsFresh(now + 30 * day, now));
    }

    if (g_fails == 0)
        std::printf("LYRICSOURCES-OK\n");
    return g_fails == 0 ? 0 : 1;
}
