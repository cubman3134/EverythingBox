// Headless test for BackgroundAudio (issue #193, increment 3): the pure rules behind music that keeps
// playing when you walk away from its now-playing page. Prints BGAUDIO-OK.
//
// WHAT IS ACTUALLY BEING PINNED. Not "audio is not video" — the interesting part is that leaving the page
// used to do FOUR things at once (stop the player, clear the queue, drop the lyric cache, tear down the page)
// and only the last of those is the page's own. Every arm below is a job the leave either still owes or must
// now stop doing, and every one of them fails SILENTLY when it is wrong: a queue kept for a film means mpv
// plays a movie's soundtrack behind the home screen; a queue dropped for an album means this increment did
// nothing; a resume route offered over an empty session opens a blank page.
//
// The oracle is written beside each case as the user-visible consequence, not as a restatement of the code.
#include "../src/media/BackgroundAudio.h"
#include <cstdio>
#include <initializer_list>   // the film/IPTV arm is stated once over both video shapes

static int fails = 0;
#define CHECK(cond, name) do { if (cond) printf("PASS %s\n", name); \
    else { printf("FAIL %s\n", name); ++fails; } } while (0)

using BackgroundAudio::Exit;
using BackgroundAudio::Reopen;
using BackgroundAudio::Session;

int main()
{
    // The three states a leave can be made from.
    const Session album { 6, false };   // a six-track music queue: the shape this increment exists for
    const Session film  { 1, true };    // a movie / a single video
    const Session iptv  { 40, true };   // a channel list: a VIDEO queue that happens to be long
    const Session idle  { 0, false };   // nothing playing

    // ---- audioLive: the one question every rule below turns on -----------------------------------------
    {
        CHECK(BackgroundAudio::audioLive(album), "live: a music queue is a live audio session");
        CHECK(BackgroundAudio::audioLive({ 1, false }), "live: so is a one-track queue");
        CHECK(!BackgroundAudio::audioLive(film), "live: a film is not");
        CHECK(!BackgroundAudio::audioLive(iptv), "live: nor is a long channel list");
        CHECK(!BackgroundAudio::audioLive(idle), "live: nor is nothing at all");
        // THE LATCH CASE, and the reason the count is asked first. mediaIsVideo_ is written by the host at
        // each open and NOTHING clears it on stop, so an empty queue left behind by a film still reads
        // `video` — and an empty queue left behind by an ALBUM still reads `audio`. The second is the
        // dangerous one here: without the count test, "the album you just stopped" would still be offered as
        // something to go back to, and picking it would open a now-playing page over no tracks.
        CHECK(!BackgroundAudio::audioLive({ 0, false }),
              "live: an EMPTY queue is not live even with the audio latch still set");
        CHECK(!BackgroundAudio::audioLive({ 0, true }), "live: nor with the video latch still set");
        CHECK(!BackgroundAudio::audioLive({ -1, false }), "live: a nonsense count reads as no queue");
    }

    // ---- planExit: what leaving the page owes ----------------------------------------------------------
    {
        const Exit e = BackgroundAudio::planExit(album);
        // The whole increment, in two booleans.
        CHECK(!e.stopPlayer, "exit/audio: the player is NOT stopped — the album keeps playing");
        CHECK(!e.clearQueue, "exit/audio: the queue survives, so increment 2's Append arm has something to "
                             "append to");
        // clearQueue is where persistResume() and the consumption flush happen (and where gapless/crossfade
        // are disarmed). NOT calling it is the same decision as "the bookkeeping runs at the real end of the
        // media", so these two flags are deliberately the same flag: a plan that kept the player but cleared
        // the queue would leave mpv playing a queue the session no longer has.
        CHECK(e.clearQueue == e.stopPlayer, "exit/audio: stopping and clearing are one decision, not two");
        CHECK(e.keepLyrics, "exit/audio: the lyric cache is keyed by the PLAYING track's path and survives "
                            "with it");
        CHECK(e.background, "exit/audio: the route back is offered");
    }
    {
        // Video and IPTV keep exiting EXACTLY as they did. This is the arm that is not allowed to change.
        for (const Session& s : { film, iptv })
        {
            const Exit e = BackgroundAudio::planExit(s);
            CHECK(e.stopPlayer, "exit/video: the player is stopped, as it always was");
            CHECK(e.clearQueue, "exit/video: the queue is cleared, as it always was");
            CHECK(!e.keepLyrics, "exit/video: the lyric cache dies with the media");
            CHECK(!e.background, "exit/video: a film is never left playing behind the UI");
        }
    }
    {
        const Exit e = BackgroundAudio::planExit(idle);
        CHECK(e.stopPlayer && e.clearQueue && !e.background,
              "exit/idle: nothing playing leaves nothing behind");
    }
    {
        // The default value IS today's behaviour, so a caller that cannot use the audio arm (goBack reached
        // from a page that is not the player) may take it without asking. Pinned because the whole point of
        // the default is that it is safe to rely on.
        const Exit d;
        CHECK(d.stopPlayer && d.clearQueue && !d.keepLyrics && !d.background,
              "exit: the default plan is the pre-#193 behaviour, exactly");
    }

    // ---- offerResume: when the route back is drawn -----------------------------------------------------
    {
        CHECK(BackgroundAudio::offerResume(album, /*pageVisible*/ false),
              "resume: offered while the album plays with its page closed");
        // On the page itself the route back is a row that does nothing. A menu of rows that do nothing is how
        // a user learns to stop opening the menu.
        CHECK(!BackgroundAudio::offerResume(album, /*pageVisible*/ true),
              "resume: NOT offered while you are standing on the page it would open");
        CHECK(!BackgroundAudio::offerResume(film, false), "resume: never for a film");
        CHECK(!BackgroundAudio::offerResume(iptv, false), "resume: never for a channel list");
        CHECK(!BackgroundAudio::offerResume(idle, false), "resume: nothing playing offers nothing");
        // The queue that played out. Its count is still zero — this is what stops "Now playing — <track>"
        // sitting in the menu naming a track that ended twenty minutes ago.
        CHECK(!BackgroundAudio::offerResume({ 0, false }, false),
              "resume: an emptied queue stops being offered");
    }

    // ---- planReopen: which surface comes back ----------------------------------------------------------
    {
        CHECK(BackgroundAudio::planReopen(album, /*wasThemed*/ true) == Reopen::ThemedPage,
              "reopen: a themed session returns to the themed now-playing page");
        CHECK(BackgroundAudio::planReopen(album, /*wasThemed*/ false) == Reopen::ClassicPlayerPage,
              "reopen: a classic session returns to the classic player page");
        // `wasThemed` is remembered from when the page OPENED, not re-derived: a theme with no
        // `nowplayingAudio` view falls back to the classic player page for the whole session, and sending it
        // to a page its theme cannot draw is a blank screen.
        CHECK(BackgroundAudio::planReopen({ 2, false }, false) == Reopen::ClassicPlayerPage,
              "reopen: a themed-mode session that fell back to classic goes back to classic");
        // THE STALE-AFFORDANCE CASE. The menu row was built while the album played; by the time it is picked
        // the queue can have played out, or another play can have taken over. Reopening a now-playing page
        // over an empty session is a blank page with no way to explain itself.
        CHECK(BackgroundAudio::planReopen(idle, true) == Reopen::Nothing,
              "reopen: a queue that ended under the menu row reopens NOTHING");
        CHECK(BackgroundAudio::planReopen(film, true) == Reopen::Nothing,
              "reopen: a video queue that took over reopens nothing either");
        CHECK(BackgroundAudio::planReopen({ 0, false }, false) == Reopen::Nothing,
              "reopen: and the same with the audio latch still set");
    }

    if (fails == 0) printf("BGAUDIO-OK\n");
    return fails == 0 ? 0 : 1;
}
