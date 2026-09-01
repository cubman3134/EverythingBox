// Headless test for PlaybackFailure (issue #228): what a file mpv could not open owes the screen.
// Prints PLAYFAIL-OK.
//
// WHAT IS ACTUALLY BEING PINNED. Not "a failure shows a message" — the app already did that, and the bug was
// filed anyway. The interesting part is that mpv reports a failed open ONCE and then goes quiet: no `pause`
// change (it never paused, it went idle), no further positions. Every surface that shows playback state is
// fed by those two signals, so unless the host corrects the state by hand, the now-playing page keeps the
// pause glyph it was given at the open and the seek bar keeps the previous item's timeline. Each arm below
// is one of those corrections, and each fails SILENTLY when it is wrong: forget showStopped and the page
// says "playing" over silence until the listener presses Stop; forget zeroTransport and a dead file borrows
// a finished one's running time; forget the gapless carve-out and one bad track in an album stops the album.
//
// The oracle is written beside each case as the user-visible consequence, not as a restatement of the code.
#include "../src/media/PlaybackFailure.h"
#include <cstdio>

static int fails = 0;
#define CHECK(cond, name) do { if (cond) printf("PASS %s\n", name); \
    else { printf("FAIL %s\n", name); ++fails; } } while (0)

using PlaybackFailure::Response;
using PlaybackFailure::Situation;

int main()
{
    // The three shapes a load failure arrives in.
    const Situation bookPage { false, true };    // an audiobook / track on the themed now-playing page
    const Situation elsewhere{ false, false };   // a film, a stream, a classic-mode play
    const Situation gapless  { true,  true };    // one entry of a gapless album mpv is already past

    // ---- The single-file failure: everything is owed ---------------------------------------------------
    {
        const Response r = PlaybackFailure::plan(bookPage);
        // THE BUG #228 WAS FILED FROM. mpv is idle and silent; without this the page draws the `pause` glyph
        // it was handed when the open began, which is its ONE live-state signal, and the listener is looking
        // at a cover and a transport that claims to be playing nothing.
        CHECK(r.showStopped, "single: the transport reads STOPPED, not playing");
        // duration_/lastPos_ are the host's copies of "how long" and "where". Nothing updates them after a
        // failed open, so they keep the PREVIOUS item's numbers — legible, and false.
        CHECK(r.zeroTransport, "single: the timeline is zeroed, not left on the last item's numbers");
        // mpv is already idle; this is about the APP's notion of a live session, which feeds the menu-music
        // question and the route back to the now-playing page.
        CHECK(r.stopPlayer, "single: the session is really stopped, so nothing counts it as live");
        // A cover, 0:00 and a dead transport is the whole screen: a message that expires leaves the listener
        // staring at exactly what they were staring at before.
        CHECK(r.noticeMs == kFeedbackSticky, "single: the message is STICKY on the audio page");
    }

    // ---- The same failure anywhere else ----------------------------------------------------------------
    {
        const Response r = PlaybackFailure::plan(elsewhere);
        CHECK(r.stopPlayer && r.zeroTransport && r.showStopped,
              "elsewhere: a film or stream owes the same three corrections");
        // Off the audio page there is a populated screen behind the notice, so the ordinary error band
        // applies — sticky there would be a message with nothing to resolve it.
        CHECK(r.noticeMs == kFeedbackLong, "elsewhere: a timed error notice, not a sticky one");
    }

    // ---- The carve-out: gapless, where the failure is NOT the end --------------------------------------
    {
        const Response r = PlaybackFailure::plan(gapless);
        // #141 feeds mpv's own playlist one entry ahead, so mpv holds two or more files and moves between
        // them itself. A failed entry is reported exactly like a fatal one — END_FILE, reason ERROR — while
        // mpv carries straight on. Every correction below would be applied OVER music that is playing fine.
        CHECK(!r.stopPlayer,     "gapless: one bad track does not stop the album");
        CHECK(!r.zeroTransport,  "gapless: nor zero the timeline of the track that is playing");
        CHECK(!r.showStopped,    "gapless: nor make a playing album read as stopped");
        // The three above are one claim — "nothing is playing" — and they must move together, or a half-
        // applied correction leaves a state no arm of this table describes.
        CHECK(r.stopPlayer == r.zeroTransport && r.zeroTransport == r.showStopped,
              "gapless: the three corrections are one claim and move together");
        // Sticky is for a screen with nothing on it. Here the album plays on, so a message that never leaves
        // would sit over the next several tracks.
        CHECK(r.noticeMs == kFeedbackLong, "gapless: a timed message, never a sticky one over playing music");
    }

    // The audio page does not change WHETHER the corrections are owed, only how long the message lives.
    // Stated on its own because the two facts are independent and it would be easy to fuse them by accident.
    CHECK(PlaybackFailure::plan(bookPage).stopPlayer == PlaybackFailure::plan(elsewhere).stopPlayer,
          "page: which surface is up decides the message length, not the state correction");
    CHECK(PlaybackFailure::plan({ true, false }).noticeMs == kFeedbackLong,
          "page: a gapless failure off the audio page is timed too");

    // The default-constructed Situation is the commonest real one (a single remote file, not on the themed
    // page), and a caller that fills in nothing must still get the full correction rather than silence.
    CHECK(PlaybackFailure::plan(Situation{}).stopPlayer && PlaybackFailure::plan(Situation{}).showStopped,
          "default: an unfilled situation still corrects the state");

    if (fails == 0) printf("PLAYFAIL-OK\n");
    return fails == 0 ? 0 : 1;
}
