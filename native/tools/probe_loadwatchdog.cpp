// Headless test for LoadWatchdog (issue #213): the rules that turn a load which says nothing into a failure.
// Prints LOADWATCHDOG-OK.
//
// WHAT IS ACTUALLY BEING PINNED. Not "a timer fires" — the interesting parts are the four ways this rule
// could be wrong and stay green: watch a live channel and turn a slow-to-open stream into an error message;
// re-grace forever and let a source that buffered one packet hang the page for good; fail a cold remote link
// at the first deadline because "mpv cannot say yet" was read as "no bytes" (the bug the first cut of this
// had, found live); or never fail a demuxer that positively reports an empty cache. Each arm below is one of
// those, and the oracle beside it is the user-visible consequence rather than a restatement of the code.
#include "../src/media/LoadWatchdog.h"
#include <cstdio>

static int fails = 0;
#define CHECK(cond, name) do { if (cond) printf("PASS %s\n", name); \
    else { printf("FAIL %s\n", name); ++fails; } } while (0)

using LoadWatchdog::Phase;
using LoadWatchdog::Progress;
using LoadWatchdog::Tick;
using LoadWatchdog::Verdict;

int main()
{
    // ---- watches: which loads the watchdog stands over -------------------------------------------------
    {
        CHECK(LoadWatchdog::watches("https://cdn.example/dld/5e5c4392-5263?token=abc"),
              "watches: a remote file link is watched");
        CHECK(LoadWatchdog::watches("C:/EverythingBox-app/audiobooks/Book/01 - Part One.mp3"),
              "watches: a local file is watched (it never stalls, and needs no special case)");
        CHECK(LoadWatchdog::watches(""), "watches: an empty url is watched rather than excused");
        // THE EXCLUSION. A live channel buffers differently, and one that is merely slow to open would be
        // turned into an error message by a watchdog that does not know what it is looking at.
        CHECK(!LoadWatchdog::watches("https://lonestar-rakuten.amagi.tv/playlist.m3u8"),
              "watches: a live .m3u8 is NOT watched");
        CHECK(!LoadWatchdog::watches("https://x.example/live/PLAYLIST.M3U8"),
              "watches: nor with the extension in capitals");
        // A signed HLS link nearly always carries a token, so the extension has to be judged on the path.
        CHECK(!LoadWatchdog::watches("https://x.example/live/index.m3u8?token=xyz&expires=1"),
              "watches: nor with a query string after the extension");
        CHECK(!LoadWatchdog::watches("https://x.example/live/index.m3u8#frag"),
              "watches: nor with a fragment after it");
        CHECK(!LoadWatchdog::watches("https://x.example/tv/channels.m3u"),
              "watches: an .m3u playlist is not watched either");
        // ...and a path that merely CONTAINS the word is a file like any other.
        CHECK(LoadWatchdog::watches("https://x.example/m3u8/episode.mp4"),
              "watches: a path segment named m3u8 is not an extension");
        CHECK(LoadWatchdog::watches("https://x.example/film.m3u8.mp4"),
              "watches: nor is .m3u8 in the middle of a filename");
    }

    // ---- judge: what a deadline means ------------------------------------------------------------------
    {
        // THE CASE FOUND LIVE, and the reason progress is three-valued. A server that sent headers and 4 KiB
        // and then hung read IDENTICALLY to one that sent nothing: until mpv has enough bytes to identify
        // the format there is no demuxer, and no demuxer property to ask. "Cannot say" is not "no bytes",
        // and a cold link that is still warming up must get its re-grace rather than be killed at 12 s.
        CHECK(LoadWatchdog::judge({ Phase::First, false, Progress::Unknown }) == Verdict::Regrace,
              "judge: first deadline, mpv cannot say yet -> REGRACE (not knowable is not dead)");
        // Slow but alive: a demuxer exists and has parsed something. One more chance.
        CHECK(LoadWatchdog::judge({ Phase::First, false, Progress::Some }) == Verdict::Regrace,
              "judge: first deadline, some progress -> REGRACE (slow, not dead)");
        // POSITIVE evidence of death: the format was identified, and nothing has come since. This is the one
        // shape that fails at the FIRST deadline, and it fails because mpv said so, not because a clock ran out.
        CHECK(LoadWatchdog::judge({ Phase::First, false, Progress::None }) == Verdict::Stall,
              "judge: first deadline, a demuxer that reports NOTHING -> STALL");
        // THE CASE A SINGLE NO-PROGRESS TEST WOULD HANG ON: it buffered a little and then died. At the second
        // deadline the bytes no longer buy anything — FILE_LOADED never came, and it never will.
        CHECK(LoadWatchdog::judge({ Phase::Second, false, Progress::Some }) == Verdict::Stall,
              "judge: second deadline, progress but still no file-loaded -> STALL");
        CHECK(LoadWatchdog::judge({ Phase::Second, false, Progress::Unknown }) == Verdict::Stall,
              "judge: second deadline, still cannot say -> STALL (32 s of nothing is enough)");
        CHECK(LoadWatchdog::judge({ Phase::Second, false, Progress::None }) == Verdict::Stall,
              "judge: second deadline, nothing -> STALL");
        // A queued timeout can land after FILE_LOADED already disarmed everything; it must be a no-op, not
        // a failure announced over a track that is playing.
        CHECK(LoadWatchdog::judge({ Phase::First, true, Progress::None }) == Verdict::Loaded,
              "judge: a late first-phase tick over a loaded file does nothing");
        CHECK(LoadWatchdog::judge({ Phase::Second, true, Progress::Some }) == Verdict::Loaded,
              "judge: nor a late second-phase one");
        // TERMINATION, stated as its own property because it is the one that keeps this from being #213 with
        // extra steps: no input in the second phase can ask for another grace.
        bool secondEverRegraces = false;
        for (bool loaded : { false, true })
            for (Progress p : { Progress::Unknown, Progress::None, Progress::Some })
                if (LoadWatchdog::judge({ Phase::Second, loaded, p }) == Verdict::Regrace)
                    secondEverRegraces = true;
        CHECK(!secondEverRegraces, "judge: the second phase never re-graces, so every load terminates");
        // The default-constructed tick is the commonest real one (first deadline, no demuxer yet), and a
        // caller that fills in nothing must get the patient answer, not the fatal one.
        CHECK(LoadWatchdog::judge(Tick{}) == Verdict::Regrace,
              "judge: an unfilled tick re-graces rather than stalls");
    }

    // ---- the deadlines and what the message says -------------------------------------------------------
    {
        CHECK(LoadWatchdog::deadlineMs(Phase::First) > 0 && LoadWatchdog::deadlineMs(Phase::Second) > 0,
              "deadline: both phases have a positive deadline");
        // The message says how long the listener waited IN TOTAL: the second deadline runs after the first,
        // so a stall declared in phase two is not "20 seconds" of nothing, it is 32.
        CHECK(LoadWatchdog::waitedSeconds(Phase::First) == LoadWatchdog::kFirstDeadlineMs / 1000,
              "waited: a first-phase stall reports the first deadline");
        CHECK(LoadWatchdog::waitedSeconds(Phase::Second)
                  == (LoadWatchdog::kFirstDeadlineMs + LoadWatchdog::kSecondDeadlineMs) / 1000,
              "waited: a second-phase stall reports the SUM, not the second deadline alone");
        CHECK(LoadWatchdog::waitedSeconds(Phase::Second) > LoadWatchdog::waitedSeconds(Phase::First),
              "waited: the total only ever grows");
    }

    if (fails == 0) printf("LOADWATCHDOG-OK\n");
    return fails == 0 ? 0 : 1;
}
