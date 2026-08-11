// Headless check of the LRC lyric parser (issue #142, source 1 — the sidecar) — the pure core pulled out of the
// player so parse + current-line lookup are pinned without a window, a file on disk or a running clock. QtCore
// only, so it runs under the offscreen QPA in CI. It pins (src/media/LrcLyrics.h, header-only, mutation-tested):
//
//   * parseLrc extracts timestamped lines in TIME ORDER, from unsorted input;
//   * a MULTI-timestamp line ([00:12.00][00:47.00]Chorus) yields the same text at EACH time;
//   * the [offset:] tag shifts EVERY line time (positive offset = earlier, so subtracted);
//   * ID tags [ti:]/[ar:]/[al:] fill title/artist/album;
//   * a file with NO timestamp is synced=false with the raw text preserved (USLT-style);
//   * enhanced word-level <mm:ss.xx> tags are stripped without dropping the line;
//   * lineIndexAtTime is −1 before the first line, the right index mid-song, and the last line past the end.
//
// Prints LYRICS-OK on success; any failure prints LYRICS-FAIL <cond> (line) and exits non-zero.
//
// FIXTURES ARE INDEPENDENT of the code under test: every LRC string is hand-written, and every expected time,
// text, index and count is a hand-computed literal — no expected value is produced by running parseLrc itself,
// so an assertion cannot pass merely because it re-ran the function it is testing.
#include "LrcLyrics.h"

#include <QCoreApplication>
#include <QString>
#include <cmath>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "LYRICS-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

static bool near(double a, double b) { return std::fabs(a - b) < 1e-9; }

namespace L = LrcLyrics;

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // --- 1. timestamped lines come out in time order, from deliberately UNSORTED input ---------------------
    // Hand-authored: three lines, written out of order (12s, 5s, 30s). Expected order: 5, 12, 30.
    {
        const QString lrc =
            QStringLiteral("[00:12.00]Second line\n"
                           "[00:05.00]First line\n"
                           "[00:30.50]Third line\n");
        const L::Lyrics ly = L::parseLrc(lrc);
        CHECK(ly.synced == true);
        CHECK(ly.lines.size() == 3);
        CHECK(near(ly.lines[0].timeSec, 5.0));   CHECK(ly.lines[0].text == QStringLiteral("First line"));
        CHECK(near(ly.lines[1].timeSec, 12.0));  CHECK(ly.lines[1].text == QStringLiteral("Second line"));
        CHECK(near(ly.lines[2].timeSec, 30.5));  CHECK(ly.lines[2].text == QStringLiteral("Third line"));

        // lineIndexAtTime across the same fixture: before the first line, mid-song, and past the end.
        CHECK(L::lineIndexAtTime(ly, 0.0)  == -1); // before the first line begins
        CHECK(L::lineIndexAtTime(ly, 4.9)  == -1); // still before line 0 (5.0)
        CHECK(L::lineIndexAtTime(ly, 5.0)  == 0);  // exactly at line 0 counts (<=)
        CHECK(L::lineIndexAtTime(ly, 11.9) == 0);  // between line 0 and 1
        CHECK(L::lineIndexAtTime(ly, 12.0) == 1);  // exactly at line 1
        CHECK(L::lineIndexAtTime(ly, 20.0) == 1);  // mid-song, between 1 and 2
        CHECK(L::lineIndexAtTime(ly, 99.0) == 2);  // past the last line -> the last index, not out of range
    }

    // --- 2. a MULTI-timestamp line yields the same text at each time (chorus repeat) -----------------------
    {
        const QString lrc = QStringLiteral("[00:12.00][00:47.00]Chorus\n");
        const L::Lyrics ly = L::parseLrc(lrc);
        CHECK(ly.synced == true);
        CHECK(ly.lines.size() == 2); // two entries from one text line
        CHECK(near(ly.lines[0].timeSec, 12.0)); CHECK(ly.lines[0].text == QStringLiteral("Chorus"));
        CHECK(near(ly.lines[1].timeSec, 47.0)); CHECK(ly.lines[1].text == QStringLiteral("Chorus"));
    }

    // --- 3. [offset:] shifts EVERY line time; positive offset is subtracted (lyrics appear earlier) ---------
    // Hand-computed: offset +500ms = 0.5s. Line at 12.000 -> 11.5; line at 20.000 -> 19.5.
    {
        const QString lrc =
            QStringLiteral("[offset:+500]\n"
                           "[00:12.00]Alpha\n"
                           "[00:20.00]Beta\n");
        const L::Lyrics ly = L::parseLrc(lrc);
        CHECK(ly.lines.size() == 2);
        CHECK(near(ly.lines[0].timeSec, 11.5)); // 12.0 - 0.5
        CHECK(near(ly.lines[1].timeSec, 19.5)); // 20.0 - 0.5
    }

    // --- 4. ID tags fill title/artist/album ----------------------------------------------------------------
    {
        const QString lrc =
            QStringLiteral("[ti:Bohemian Rhapsody]\n"
                           "[ar:Queen]\n"
                           "[al:A Night at the Opera]\n"
                           "[00:00.00]Is this the real life\n");
        const L::Lyrics ly = L::parseLrc(lrc);
        CHECK(ly.title  == QStringLiteral("Bohemian Rhapsody"));
        CHECK(ly.artist == QStringLiteral("Queen"));
        CHECK(ly.album  == QStringLiteral("A Night at the Opera"));
        CHECK(ly.synced == true);
        CHECK(ly.lines.size() == 1);
        CHECK(ly.lines[0].text == QStringLiteral("Is this the real life"));
    }

    // --- 5. a file with NO timestamp is unsynced, raw text preserved (USLT-style) ---------------------------
    {
        const QString lrc =
            QStringLiteral("Just a plain lyric sheet\n"
                           "with no timing at all\n"
                           "three lines of it\n");
        const L::Lyrics ly = L::parseLrc(lrc);
        CHECK(ly.synced == false);
        CHECK(ly.lines.size() == 3);
        CHECK(ly.lines[0].text == QStringLiteral("Just a plain lyric sheet"));
        CHECK(ly.lines[1].text == QStringLiteral("with no timing at all"));
        CHECK(ly.lines[2].text == QStringLiteral("three lines of it"));
    }

    // --- 6. enhanced word-level <mm:ss.xx> tags are stripped, the line kept whole --------------------------
    {
        const QString lrc = QStringLiteral("[00:10.00]<00:10.00>Hello <00:10.50>cruel <00:11.00>world\n");
        const L::Lyrics ly = L::parseLrc(lrc);
        CHECK(ly.synced == true);
        CHECK(ly.lines.size() == 1);
        CHECK(near(ly.lines[0].timeSec, 10.0));
        CHECK(ly.lines[0].text == QStringLiteral("Hello cruel world")); // word tags gone, text intact
    }

    // --- 7. millisecond (3-digit) and bare (no-frac) timestamps parse -------------------------------------
    {
        const QString lrc =
            QStringLiteral("[01:02.250]Milli\n"   // 62.25s
                           "[02:00]Bare\n");        // 120s, no fraction
        const L::Lyrics ly = L::parseLrc(lrc);
        CHECK(ly.lines.size() == 2);
        CHECK(near(ly.lines[0].timeSec, 62.25));
        CHECK(near(ly.lines[1].timeSec, 120.0));
    }

    // --- 8. empty / whitespace input is best-effort, never a crash ----------------------------------------
    {
        const L::Lyrics ly = L::parseLrc(QString());
        CHECK(ly.synced == false);
        CHECK(ly.lines.isEmpty());
        CHECK(L::lineIndexAtTime(ly, 5.0) == -1); // no lines -> no current line
    }

    if (failures == 0)
        std::printf("LYRICS-OK\n");
    else
        std::fprintf(stderr, "LYRICS had %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
