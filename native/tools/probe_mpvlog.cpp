// Headless check of the mpv log capture (issue #231): the two pure pieces the capture is built out of —
// core/LogSafeText.h (what may be written down) and video/MpvLogThrottle.h (how much of it may be). QtCore
// only: both are header-only and neither touches libmpv, a window or a GPU, so this runs under the offscreen
// QPA in CI beside the rest of the suite.
//
// WHAT THIS PROBE CAN AND CANNOT SEE. It cannot see that MpvWidget calls mpv_request_log_messages, and it
// cannot make a real decoder emit a real concealment warning — the acceptance evidence for #231 is a live run
// against a deliberately truncated file, and no unit test substitutes for it. What it CAN pin, and what would
// otherwise be pinned by nothing, is the pair of properties that make the capture safe to ship on by default:
//
//   1. A CREDENTIAL CANNOT REACH THE FILE. mpv logs urls verbatim — "Opening https://…?token=…" is an
//      ordinary `v`-level line — and stream_debug.log is the file people paste into bug reports. #200-#204
//      removed signed urls from five places that treated them as text; this is the sixth, and it must not be
//      the one that puts them back. Asserted by NAME (the token text is absent) and by RULE
//      (StoredUrl::carriesCredential is false for the result), because either alone can be satisfied by an
//      accident.
//
//   2. A FLOOD CANNOT DESTROY THE LOG. A damaged HEVC stream emits a concealment warning per frame. main.cpp
//      caps stream_debug.log at 1 MB and DELETES it above that, so an unlimited capture would take the rest
//      of the session's evidence with it — the failure would be worse than the silence #231 is fixing. The
//      throttle's contract is asserted arithmetically: every message is either written or counted, never
//      neither, and the counts in the summaries add up to exactly the number suppressed.
//
// Prints MPVLOG-OK on success; any failure prints MPVLOG-FAIL <cond> (line) and exits non-zero.
#include "LogSafeText.h"
#include "StoredUrl.h"
#include "MpvLogThrottle.h"
#include "MpvLogLevel.h"

#include <QCoreApplication>
#include <QRegularExpression>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "MPVLOG-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

#define EXPECT_EQ(got, want) do { \
    const QString g_ = (got), w_ = (want); \
    if (g_ != w_) { \
        std::fprintf(stderr, "MPVLOG-FAIL got '%s' want '%s' (line %d)\n", \
                     g_.toUtf8().constData(), w_.toUtf8().constData(), __LINE__); \
        ++failures; \
    } \
} while (0)

// A token spelled out here once, so every assertion below can look for THIS string and nothing else.
//
// DELIBERATELY NOT JWT-SHAPED. The first draft of this probe used a realistic three-part signed-JWT
// literal and GitGuardian failed the PR for it — correctly: a repository should not carry a string a secret
// scanner cannot tell from a real credential, and "it is only a fixture" is precisely what every real leak
// says. What the scrub actually has to cope with is an opaque, high-entropy run in a query or a path
// segment; the base64 JWT header adds nothing to that and costs a red check on every future PR that touches
// this file. So the fixture says out loud what it is.
static const char* kToken = "FIXTURE-NOT-A-SECRET-231-aaaabbbbccccddddeeeeffff0000";

// ---- 1. THE SCRUB ---------------------------------------------------------------------------------------

// The rule applied to a string that IS a url, checked against hand-written expectations. These are the same
// renderings the three logSafeUrl() statics produced before #231 folded them into one definition — that is
// deliberate, and it is what makes this probe a guard on the de-duplication as well as on the new caller.
static void testUrlRule()
{
    EXPECT_EQ(LogSafeText::url(QStringLiteral("https://cdn.example.net/dl/9f2c/Movie.mkv")),
              QStringLiteral("https://cdn.example.net/…/Movie.mkv"));
    // The query goes, which is where a signed-url token rides.
    EXPECT_EQ(LogSafeText::url(QStringLiteral("https://cdn.example.net/dl/Movie.mkv?token=abc&exp=1756742400")),
              QStringLiteral("https://cdn.example.net/…/Movie.mkv"));
    // Userinfo goes — the one path-adjacent credential with an unambiguous syntax (StoredUrl's rule).
    EXPECT_EQ(LogSafeText::url(QStringLiteral("http://user:pw@host.example/a/b/c.ts")),
              QStringLiteral("http://host.example/…/c.ts"));
    // A port survives: which port a stream came from is diagnosis, not a secret.
    EXPECT_EQ(LogSafeText::url(QStringLiteral("http://192.168.1.9:8096/Videos/x/stream.mkv?api_key=k")),
              QStringLiteral("http://192.168.1.9:8096/…/stream.mkv"));
    // The path's MIDDLE segments go too — the step StoredUrl::location deliberately declines to take for a
    // STORED url (it has to stay re-openable) and this rule takes because a log line does not.
    CHECK(!LogSafeText::url(QStringLiteral("https://h.example/dl/") + QLatin1String(kToken)
                            + QStringLiteral("/Movie.mkv")).contains(QLatin1String(kToken)));
    // A local path is reduced to its file name, as the three statics always did.
    EXPECT_EQ(LogSafeText::url(QStringLiteral("/home/me/Videos/My File.mkv")), QStringLiteral("My File.mkv"));
}

// Free text with a url somewhere in it — the actual shape of an mpv message.
static void testScrubInProse()
{
    const QString signed_ = QStringLiteral("https://cdn.example.net/dl/Movie.mkv?token=") + QLatin1String(kToken);

    // The `v`-level line that made redaction non-optional here.
    const QString opening = LogSafeText::scrub(QStringLiteral("[stream] Opening ") + signed_);
    CHECK(!opening.contains(QLatin1String(kToken)));
    EXPECT_EQ(opening, QStringLiteral("[stream] Opening https://cdn.example.net/…/Movie.mkv"));

    // Quoted, with the sentence continuing after it: the quotes and the full stop belong to the prose and
    // must come back, and nothing after the url may be eaten.
    const QString quoted = LogSafeText::scrub(QStringLiteral("[file] Cannot open '") + signed_
                                              + QStringLiteral("': Permission denied."));
    CHECK(!quoted.contains(QLatin1String(kToken)));
    EXPECT_EQ(quoted, QStringLiteral("[file] Cannot open 'https://cdn.example.net/…/Movie.mkv': Permission denied."));

    // Trailing sentence punctuation is handed back rather than swallowed into the url.
    EXPECT_EQ(LogSafeText::scrub(QStringLiteral("see http://h.example/a/b.ts?k=1).")),
              QStringLiteral("see http://h.example/…/b.ts)."));

    // TWO urls in one message, both scrubbed, the text between them untouched.
    const QString two = LogSafeText::scrub(QStringLiteral("redirect ") + signed_ + QStringLiteral(" -> ")
                                           + signed_ + QStringLiteral(" gave 403"));
    CHECK(!two.contains(QLatin1String(kToken)));
    EXPECT_EQ(two, QStringLiteral("redirect https://cdn.example.net/…/Movie.mkv -> "
                                  "https://cdn.example.net/…/Movie.mkv gave 403"));

    // THE RULE, not just the name: whatever survives must be something StoredUrl agrees carries no credential.
    // Checked over the scrubbed line by pulling each url back out of it.
    static const QRegularExpression rx(QStringLiteral("[a-z][a-z0-9+.-]*://[^\\s'\"]+"));
    for (const QString& line : { opening, quoted, two })
    {
        auto it = rx.globalMatch(line);
        int seen = 0;
        while (it.hasNext()) { CHECK(!StoredUrl::carriesCredential(it.next().captured())); ++seen; }
        CHECK(seen > 0);
    }

    // A message with NO url is returned byte for byte — the overwhelmingly common case, and the one where a
    // clever scrub mangling a decoder's own words would be a fault of its own.
    const QString plain = QStringLiteral("hevc: concealing 3600 DC, 3600 AC, 3600 MV errors in P frame");
    EXPECT_EQ(LogSafeText::scrub(plain), plain);
    // Neither is a bare "://" that is not a url (a codec's own prose, a windows path with a drive letter).
    EXPECT_EQ(LogSafeText::scrub(QStringLiteral("bad :// in header")), QStringLiteral("bad :// in header"));
    EXPECT_EQ(LogSafeText::scrub(QStringLiteral("reading C:\\Users\\me\\Videos\\a.mkv")),
              QStringLiteral("reading C:\\Users\\me\\Videos\\a.mkv"));
}

// ---- 2. THE THROTTLE ------------------------------------------------------------------------------------

// The message shape, which is what the burst counter buckets on. The numbers in a concealment warning change
// every frame; if they were part of the key the throttle would bucket nothing and limit nothing.
static void testShape()
{
    const QString a = QStringLiteral("hevc: concealing 3600 DC, 3600 AC, 3600 MV errors in P frame");
    const QString b = QStringLiteral("hevc: concealing 12 DC, 4 AC, 90 MV errors in P frame");
    const QString c = QStringLiteral("hevc: Could not find ref with POC 47");
    CHECK(MpvLogThrottle::shapeOf(a) == MpvLogThrottle::shapeOf(b));
    CHECK(MpvLogThrottle::shapeOf(a) != MpvLogThrottle::shapeOf(c));
    EXPECT_EQ(MpvLogThrottle::shapeOf(a), QStringLiteral("hevc: concealing # DC, # AC, # MV errors in P frame"));
}

// The contract: nothing is dropped without being counted. A frame-rate flood of ONE shape, over four
// 30-second windows, and every message is accounted for.
static void testBurstAndSummary()
{
    MpvLogThrottle t(/*burst*/4, /*windowMs*/30000);
    int written = 0, summaries = 0, reported = 0;
    static const QRegularExpression rx(QStringLiteral("^… and (\\d+) more in (\\d+) s, last: "));

    // 24 messages a second for two minutes — a real damaged-HEVC rate, 2,880 messages.
    for (int i = 0; i < 2880; ++i)
    {
        const qint64 now = qint64(i) * 1000 / 24;
        const QString msg = QStringLiteral("hevc: concealing %1 DC, %1 AC, %1 MV errors in P frame").arg(3600 - i);
        for (const QString& out : t.admit(msg, now))
        {
            const auto m = rx.match(out);
            if (m.hasMatch()) { ++summaries; reported += m.captured(1).toInt(); }
            else { ++written; CHECK(out == msg || out.startsWith(QStringLiteral("hevc: concealing"))); }
        }
    }
    for (const QString& out : t.drain(2880 * 1000 / 24))
    {
        const auto m = rx.match(out);
        CHECK(m.hasMatch());
        if (m.hasMatch()) { ++summaries; reported += m.captured(1).toInt(); }
    }

    // Four windows of 30 s in two minutes, so four bursts of four verbatim lines...
    CHECK(written == 16);
    // ...four summaries, one per window (the last one flushed by drain)...
    CHECK(summaries == 4);
    // ...and NOTHING unaccounted for: every message was either written or counted, exactly once.
    CHECK(written + reported == 2880);
    // The whole flood therefore cost twenty lines, not two thousand eight hundred and eighty.
    CHECK(written + summaries == 20);
}

// Two different shapes get independent allowances — a stream emitting both a concealment warning and a
// reference-picture warning must not have one of them starved by the other's volume.
static void testShapesAreIndependent()
{
    MpvLogThrottle t(/*burst*/4, /*windowMs*/30000);
    int conceal = 0, ref = 0;
    for (int i = 0; i < 500; ++i)
    {
        const qint64 now = qint64(i) * 10;   // well inside one window
        for (const QString& out : t.admit(QStringLiteral("hevc: concealing %1 DC errors").arg(i), now))
            if (out.startsWith(QStringLiteral("hevc: concealing"))) ++conceal;
        for (const QString& out : t.admit(QStringLiteral("hevc: Could not find ref with POC %1").arg(i), now))
            if (out.startsWith(QStringLiteral("hevc: Could not"))) ++ref;
    }
    CHECK(conceal == 4);
    CHECK(ref == 4);
    CHECK(t.trackedShapes() == 2);
}

// The summary is only ever a FOOTNOTE to a burst: a shape that arrives a handful of times is written in full
// and produces no summary at all, so the ordinary case reads exactly as it did before #231.
static void testQuietStreamIsUnchanged()
{
    MpvLogThrottle t(/*burst*/4, /*windowMs*/30000);
    QStringList out;
    out += t.admit(QStringLiteral("[stream] Opening https://h.example/…/a.mkv"), 0);
    out += t.admit(QStringLiteral("[cplayer] Video --vid=1 (*) (hevc 1920x1080)"), 5);
    out += t.admit(QStringLiteral("[ffmpeg/demuxer] http: Will reconnect at 1048576"), 90000);
    out += t.drain(120000);
    CHECK(out.size() == 3);
    CHECK(!t.pending());
    // ...and a burst that HAS been counted says so through pending(), which is what keeps the flush timer
    // running only while there is something outstanding.
    for (int i = 0; i < 50; ++i) t.admit(QStringLiteral("x %1").arg(i), 0);
    CHECK(t.pending());
}

// The shape ceiling. A source that puts a unique un-foldable string in every message would otherwise grow the
// bucket map for the length of the session; eviction is bounded AND still reports what it retires.
static void testShapeCeilingIsBounded()
{
    MpvLogThrottle t(/*burst*/1, /*windowMs*/30000, /*maxShapes*/8);
    int reported = 0, written = 0;
    static const QRegularExpression rx(QStringLiteral("^… and (\\d+) more in "));
    for (int i = 0; i < 200; ++i)
        for (const QString& out : t.admit(QStringLiteral("job %1 failed").arg(QChar('a' + i % 40)), i))
        {
            const auto m = rx.match(out);
            if (m.hasMatch()) reported += m.captured(1).toInt(); else ++written;
        }
    CHECK(t.trackedShapes() <= 8);
    for (const QString& out : t.drain(200))
    {
        const auto m = rx.match(out);
        if (m.hasMatch()) reported += m.captured(1).toInt();
    }
    CHECK(written + reported == 200);   // eviction counts too; it is not a silent drop
}

// The two halves are used together, in this order, by MpvWidget::handleLogMessage — and the order matters:
// an unscrubbed url puts a per-request signature into the throttle's KEY, so every message gets its own
// bucket and the rate limiting silently does nothing. Pinned here because nothing else would notice.
static void testScrubBeforeThrottle()
{
    // A ceiling above the message count, so what is measured here is the BUCKETING and not the eviction that
    // the default ceiling would otherwise impose at 64 (testShapeCeilingIsBounded pins that separately).
    MpvLogThrottle raw(4, 30000, 256), scrubbed(4, 30000, 256);
    for (int i = 0; i < 100; ++i)
    {
        // A per-request signature that is LETTERS, not digits — which is what a real signed url carries and
        // is exactly the case shapeOf() cannot fold away. (A numeric one would collapse on its own and the
        // assertion below would pass for the wrong reason.)
        const QString sig = QString(QChar('a' + i % 26)) + QChar('a' + (i / 26) % 26);
        const QString msg = QStringLiteral("[stream] Opening https://cdn.example.net/dl/Movie.mkv?token=") + sig;
        raw.admit(msg, i);
        scrubbed.admit(LogSafeText::scrub(msg), i);
    }
    CHECK(raw.trackedShapes() == 100);      // the wrong order: a hundred buckets, nothing limited
    CHECK(scrubbed.trackedShapes() == 1);   // the right one: one shape, throttled
}

// ---- 3. THE LEVEL RULE ----------------------------------------------------------------------------------

// The trap this rule exists for, stated as an assertion: the message #231 was filed to capture is one mpv
// reports at `v`, NOT at `warn`, because mpv demotes everything libav logs at INFO. The obvious reading of
// the issue — request "warn" — throws it away. Both halves are pinned here from the REAL text of the line
// that came off the acceptance run, so an edit that "tidies" the default back to warn fails in CI.
static void testLevelRule()
{
    // The default asked of libmpv is `v`, and an explicit EB_MPV_LOG replaces it verbatim.
    CHECK(MpvLogLevel::requested(QByteArray()) == QByteArrayLiteral("v"));
    CHECK(MpvLogLevel::requested(QByteArrayLiteral(" debug ")) == QByteArrayLiteral("debug"));

    // THE LINE. Verbatim from stream_debug.log on the truncated-packet run, level and prefix included.
    CHECK(MpvLogLevel::keep("v", "ffmpeg/video", false));      // concealing 5776 DC, 5776 AC, … in B frame
    CHECK(MpvLogLevel::keep("v", "ffmpeg/demuxer", false));    // http: Will reconnect at …
    CHECK(MpvLogLevel::keep("error", "ffmpeg/video", false));  // h264: Invalid NAL unit size (…)
    CHECK(MpvLogLevel::keep("warn", "vd", false));             // Error while decoding frame!
    CHECK(MpvLogLevel::keep("fatal", "cplayer", false));

    // ...and the 101 lines a HEALTHY open costs at full `v`, which is what the rule is buying back. These
    // are mpv's own modules at verbose: they say nothing about the stream and would rotate the 1 MB log away.
    CHECK(!MpvLogLevel::keep("v", "libmpv_render", false));    // Testing FBO format rgba16f
    CHECK(!MpvLogLevel::keep("v", "vd", false));               // Requesting 16 threads for decoding.
    CHECK(!MpvLogLevel::keep("v", "autoconvert", false));      // dropping request due to pin disconnect
    CHECK(!MpvLogLevel::keep("info", "cplayer", false));
    CHECK(!MpvLogLevel::keep("status", "cplayer", false));

    // EB_PERF / EB_MPV_LOG drop the prefix half: everything at the level libmpv was asked for.
    CHECK(MpvLogLevel::keep("v", "libmpv_render", true));
    CHECK(MpvLogLevel::keep("debug", "autoconvert", true));
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    testLevelRule();
    testUrlRule();
    testScrubInProse();
    testShape();
    testBurstAndSummary();
    testShapesAreIndependent();
    testQuietStreamIsUnchanged();
    testShapeCeilingIsBounded();
    testScrubBeforeThrottle();
    if (failures == 0) std::printf("MPVLOG-OK\n");
    return failures == 0 ? 0 : 1;
}
