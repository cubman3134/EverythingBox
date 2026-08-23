// Headless check of the lyric SEEK + OFFSET arithmetic (issue #142, the Presentation half) — the pure core
// pulled out of both surfaces so "which second does selecting this line seek to" and "what does a ±0.5 s nudge
// do to the highlight" are pinned without a window, a file on disk or a running clock. QtCore only, so it runs
// under the offscreen QPA in CI. It pins (src/media/LyricSeek.h, header-only, mutation-tested):
//
//   * canSeek — a SYNCED set with lines can be seeked to; an UNSYNCED sheet cannot, and neither can an empty
//     one. This is the gate both surfaces ask before they offer the gesture at all: without it every line of a
//     USLT sheet (whose times are all 0.0) would seek to 0:00;
//   * seekTarget — selecting line i seeks to line i's time, PLUS the offset, floored at 0, and −1 (never 0.0)
//     for a line index outside the set or a set that cannot be seeked;
//   * lineAt — the current line with an offset applied, and the direction it moves in: a POSITIVE offset makes
//     the lyrics appear LATER, so at a fixed position it selects an EARLIER line (or none);
//   * the two must agree — seeking to the line lineAt reports must land back on that same line, at any offset.
//     That round trip is the one property a split-brain sign error cannot satisfy;
//   * clampOffset / nudge — the ±0.5 s grid, the ±30 s clamp that stops at the rail rather than wrapping, and
//     the NaN / off-grid / negative-zero inputs a hand-edited ini can produce;
//   * describe — the one spelling both surfaces show.
//
// Prints LYRICSEEK-OK on success; any failure prints LYRICSEEK-FAIL <cond> (line) and exits non-zero.
//
// FIXTURES ARE INDEPENDENT of the code under test: the lyric sets are hand-built LyricLine literals (they do
// NOT come from parseLrc — that is probe_lyrics' subject and using it here would couple two probes), and every
// expected index, second and string is a hand-computed literal, so no assertion can pass merely because it
// re-ran the function it is testing.
#include "LyricSeek.h"

#include <QCoreApplication>
#include <QString>
#include <cmath>
#include <cstdio>
#include <limits>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "LYRICSEEK-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

static bool near(double a, double b) { return std::fabs(a - b) < 1e-9; }

namespace S = LyricSeek;

// A hand-built SYNCED set: four lines at 5 / 12 / 30.5 / 60 seconds. Built as literals, not parsed.
static LrcLyrics::Lyrics syncedFixture()
{
    LrcLyrics::Lyrics ly;
    ly.synced = true;
    ly.lines.push_back({ 5.0,  QStringLiteral("Line A") });
    ly.lines.push_back({ 12.0, QStringLiteral("Line B") });
    ly.lines.push_back({ 30.5, QStringLiteral("Line C") });
    ly.lines.push_back({ 60.0, QStringLiteral("Line D") });
    return ly;
}

// A hand-built UNSYNCED sheet: three lines of plain text, every time 0.0 — exactly what LrcLyrics produces for
// a USLT tag or a plain-text .lrc.
static LrcLyrics::Lyrics unsyncedFixture()
{
    LrcLyrics::Lyrics ly;
    ly.synced = false;
    ly.lines.push_back({ 0.0, QStringLiteral("Plain one") });
    ly.lines.push_back({ 0.0, QStringLiteral("Plain two") });
    ly.lines.push_back({ 0.0, QStringLiteral("Plain three") });
    return ly;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    const LrcLyrics::Lyrics synced   = syncedFixture();
    const LrcLyrics::Lyrics unsynced = unsyncedFixture();

    // --- 1. canSeek: only a synced, non-empty set --------------------------------------------------------
    {
        CHECK(S::canSeek(synced)   == true);
        CHECK(S::canSeek(unsynced) == false);       // no timestamps: every line would seek to the same second
        LrcLyrics::Lyrics emptySynced; emptySynced.synced = true;   // flagged synced but carrying no lines
        CHECK(S::canSeek(emptySynced) == false);
        CHECK(S::canSeek(LrcLyrics::Lyrics()) == false);            // the default (nothing resolved) set
    }

    // --- 2. seekTarget at zero offset: the line's own time, and −1 outside the set ------------------------
    {
        CHECK(near(S::seekTarget(synced, 0, 0.0), 5.0));
        CHECK(near(S::seekTarget(synced, 1, 0.0), 12.0));
        CHECK(near(S::seekTarget(synced, 2, 0.0), 30.5));
        CHECK(near(S::seekTarget(synced, 3, 0.0), 60.0));
        // Out of range and "no such thing to seek to" are −1, NOT 0.0 — the callers must be able to tell
        // "there is no answer" from "the answer is the start of the track".
        CHECK(near(S::seekTarget(synced, -1, 0.0), -1.0));
        CHECK(near(S::seekTarget(synced, 4, 0.0), -1.0));
        CHECK(near(S::seekTarget(unsynced, 1, 0.0), -1.0));         // an unsynced sheet offers no seek at all
        CHECK(near(S::seekTarget(unsynced, 0, 2.0), -1.0));         // ...not even with an offset applied
    }

    // --- 3. seekTarget WITH an offset: the effective time, floored at zero --------------------------------
    {
        CHECK(near(S::seekTarget(synced, 1, 0.5),  12.5));   // +0.5 s: the line is half a second later
        CHECK(near(S::seekTarget(synced, 1, -0.5), 11.5));   // −0.5 s: half a second earlier
        CHECK(near(S::seekTarget(synced, 2, 2.0),  32.5));
        // A negative offset larger than an early line's own time would ask for a position before the file
        // starts; that floors at 0 rather than handing mpv a negative seek.
        CHECK(near(S::seekTarget(synced, 0, -5.0), 0.0));
        CHECK(near(S::seekTarget(synced, 0, -9.0), 0.0));
        // The offset applied is the CLAMPED one: an off-grid value snaps before it is added.
        CHECK(near(S::seekTarget(synced, 1, 0.6), 12.5));    // 0.6 snaps to 0.5
        CHECK(near(S::seekTarget(synced, 1, 40.0), 42.0));   // 40 clamps to 30
    }

    // --- 4. lineAt at zero offset: boundaries, and no current line before the first -----------------------
    {
        CHECK(S::lineAt(synced, 0.0,  0.0) == -1);   // before the first line begins
        CHECK(S::lineAt(synced, 4.99, 0.0) == -1);
        CHECK(S::lineAt(synced, 5.0,  0.0) == 0);    // exactly at a line counts (<=)
        CHECK(S::lineAt(synced, 11.99, 0.0) == 0);
        CHECK(S::lineAt(synced, 12.0, 0.0) == 1);
        CHECK(S::lineAt(synced, 59.99, 0.0) == 2);
        CHECK(S::lineAt(synced, 120.0, 0.0) == 3);   // past the last line -> the last index, not out of range
        // An unsynced sheet has NO current line, ever. Every one of its times is 0.0, so a raw lookup would
        // report the LAST line from the first second on and the whole sheet would render as "current".
        CHECK(S::lineAt(unsynced, 0.0, 0.0)  == -1);
        CHECK(S::lineAt(unsynced, 30.0, 0.0) == -1);
    }

    // --- 5. lineAt WITH an offset: the direction the nudge moves the highlight ----------------------------
    // The whole point of the feature: the words are running AHEAD of the singer, so you push them LATER with a
    // POSITIVE offset — and at a fixed playback position that selects an EARLIER line (line B's effective time
    // moves from 12.0 to 12.5, so at 12.2 the current line is still A).
    {
        CHECK(S::lineAt(synced, 12.2, 0.0)  == 1);   // no nudge: B is current
        CHECK(S::lineAt(synced, 12.2, 0.5)  == 0);   // +0.5 s (later): B has not arrived yet, A is current
        CHECK(S::lineAt(synced, 11.8, -0.5) == 1);   // −0.5 s (earlier): B has already arrived
        CHECK(S::lineAt(synced, 4.8,  -0.5) == 0);   // the first line arrives early too
        CHECK(S::lineAt(synced, 4.8,   0.5) == -1);  // ...and can be pushed past the position entirely
        // A big positive nudge can push the whole sheet past an early position: no current line at all.
        CHECK(S::lineAt(synced, 5.5, 2.0) == -1);
        // The offset applied is the CLAMPED one here too, so the highlight and the seek agree about it.
        CHECK(S::lineAt(synced, 12.2, 0.6) == 0);
    }

    // --- 6. THE ROUND TRIP: seek to the line lineAt reports, and lineAt reports the same line -------------
    // The one property that a sign error in either half cannot satisfy, at every offset on the grid across the
    // whole fixture. If seekTarget added the offset and lineAt also added it (instead of subtracting the
    // position by it), this loop fails at every non-zero offset — which is exactly the mutation it exists for.
    {
        for (int step = -4; step <= 4; ++step)
        {
            const double off = step * S::kStepSec;
            for (int i = 0; i < synced.lines.size(); ++i)
            {
                const double t = S::seekTarget(synced, i, off);
                CHECK(t >= 0.0);
                // Floored targets are the one exception and they are honest about it: line 0 with a −2.0 s
                // nudge sits at 0.0, which is BEFORE its effective time (3.0), so the position genuinely is
                // not inside that line yet. Skip only that case; every other target must round-trip.
                if (near(t, 0.0) && synced.lines[i].timeSec + off > 0.0) continue;
                CHECK(S::lineAt(synced, t, off) == i);
                // ...and a hair before it is NOT that line (the boundary is exactly at the effective time).
                if (t > 0.0) CHECK(S::lineAt(synced, t - 0.001, off) == i - 1);
            }
        }
    }

    // --- 7. clampOffset: the ±0.5 s grid, the ±30 s rail, and the inputs an ini can hold ------------------
    {
        CHECK(near(S::kStepSec, 0.5));
        CHECK(near(S::kMaxOffsetSec, 30.0));
        CHECK(near(S::clampOffset(0.0), 0.0));
        CHECK(near(S::clampOffset(0.5), 0.5));
        CHECK(near(S::clampOffset(-2.5), -2.5));
        CHECK(near(S::clampOffset(0.6), 0.5));      // snaps DOWN to the nearest grid point
        CHECK(near(S::clampOffset(0.8), 1.0));      // ...and UP
        CHECK(near(S::clampOffset(-0.3), -0.5));    // negative snaps away from zero at the half-way point
        CHECK(near(S::clampOffset(-0.2), 0.0));
        CHECK(near(S::clampOffset(31.0), 30.0));    // the rail
        CHECK(near(S::clampOffset(-31.0), -30.0));
        CHECK(near(S::clampOffset(1e9), 30.0));
        CHECK(near(S::clampOffset(std::numeric_limits<double>::quiet_NaN()), 0.0));
        // −0.0 folds to +0.0: it reads back as "an offset is set" through any sign test and prints "−0.0 s".
        CHECK(!std::signbit(S::clampOffset(-0.0)));
        CHECK(!std::signbit(S::clampOffset(-0.1)));
    }

    // --- 8. nudge: half a second per step, stopping at the rail rather than wrapping ----------------------
    {
        CHECK(near(S::nudge(0.0, +1), 0.5));
        CHECK(near(S::nudge(0.0, -1), -0.5));
        CHECK(near(S::nudge(0.5, +1), 1.0));
        CHECK(near(S::nudge(-0.5, +1), 0.0));
        CHECK(near(S::nudge(0.0, +4), 2.0));        // several steps at once
        // Walking to the rail and pressing again parks there. A wrap would jump a listener from +30 s to
        // −30 s on one keypress, which is the worst possible response to "still not quite right".
        CHECK(near(S::nudge(30.0, +1), 30.0));
        CHECK(near(S::nudge(-30.0, -1), -30.0));
        CHECK(near(S::nudge(29.5, +1), 30.0));
        CHECK(near(S::nudge(30.0, -1), 29.5));      // ...and it comes back off the rail normally
        // A stored off-grid value is snapped BEFORE the step, so one nudge lands on the grid rather than
        // carrying the corruption forward for ever.
        CHECK(near(S::nudge(0.3, +1), 1.0));        // 0.3 -> 0.5, then +0.5
        CHECK(near(S::nudge(0.0, 0), 0.0));         // a zero step is a no-op, not a nudge
    }

    // --- 9. describe: the one spelling both surfaces show --------------------------------------------------
    {
        CHECK(S::describe(0.0)  == QStringLiteral("0 s"));
        CHECK(S::describe(-0.0) == QStringLiteral("0 s"));
        CHECK(S::describe(0.5)  == QStringLiteral("+0.5 s"));
        CHECK(S::describe(2.0)  == QStringLiteral("+2.0 s"));
        CHECK(S::describe(-1.5) == QString::fromUtf8("\xE2\x88\x92") + QStringLiteral("1.5 s"));
        CHECK(S::describe(0.6)  == QStringLiteral("+0.5 s"));   // shows what is STORED, not what was asked
    }

    if (failures == 0)
        std::printf("LYRICSEEK-OK\n");
    else
        std::fprintf(stderr, "LYRICSEEK had %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
