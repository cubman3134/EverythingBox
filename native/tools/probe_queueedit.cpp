// Headless test for QueueEdit (issue #193): the pure index arithmetic behind editing a queue you are
// listening to. Prints QUEUEEDIT-OK.
//
// WHAT IS ACTUALLY BEING PINNED. Not "the list gets shorter" — QList already does that. The thing that can
// silently break is the SECOND answer every edit has to give: under gapless (#141, on by default) mpv holds
// its own playlist fed one entry ahead, so an edit that lands on the entry mpv has already been handed leaves
// the two disagreeing, and the symptom is the wrong song rather than a crash or a failed assertion. Every
// `reseat` assertion below is that answer, and each one is stated against a hand-computed permutation written
// out in the comment beside it, so the probe's oracle is independent of the function's own body.
//
// The state is (count, cursor, frontier); mpv holds, un-played, exactly the queue indices in
// (cursor, frontier]. See QueueEdit.h for why that is the whole model.
#include "../src/media/QueueEdit.h"
#include <cstdio>

static int fails = 0;
#define CHECK(cond, name) do { if (cond) printf("PASS %s\n", name); \
    else { printf("FAIL %s\n", name); ++fails; } } while (0)

using QueueEdit::Plan;
using QueueEdit::State;

int main()
{
    // The live shape this feature exists for: five tracks, playing #1, and mpv has already been handed #2.
    const State live { 5, 1, 2 };

    // ---- inserting -------------------------------------------------------------------------------------
    // PLAY NEXT is the edit that always crosses the frontier, and it is why the reseat exists at all: mpv was
    // handed old-#2 and the app now says the new entry comes next. [0,1*,2,3,4] -> [0,1*,N,2,3,4]
    {
        const Plan p = QueueEdit::planInsert(live, 2, 1);
        CHECK(p.valid && p.count == 6, "insert: play-next grows the queue by one");
        CHECK(p.cursor == 1, "insert at cursor+1 leaves the playing track where it is");
        CHECK(p.reseat, "insert at cursor+1 CROSSES the gapless frontier — mpv must be re-seated");
        CHECK(p.frontier == 1, "a crossing insert shrinks the frontier back to the playing entry");
        CHECK(!p.cursorRemoved, "an insert never removes the playing track");
    }
    // One slot further down is BELOW the frontier in queue order but PAST the entry mpv holds: mpv's next is
    // still old-#2, and old-#2 is still the app's next. Nothing to repair. [0,1*,2,3,4] -> [0,1*,2,N,3,4]
    {
        const Plan p = QueueEdit::planInsert(live, 3, 1);
        CHECK(p.valid && !p.reseat, "insert past the frontier leaves mpv alone");
        CHECK(p.cursor == 1 && p.frontier == 2, "…and renumbers neither the cursor nor the frontier");
    }
    // ABOVE the cursor: everything from the insert point down shifts by one TOGETHER, cursor included, so the
    // entry mpv holds is still the entry after the one playing. [0,1*,2,3,4] -> [N,0,1*,2,3,4]
    {
        const Plan p = QueueEdit::planInsert(live, 0, 1);
        CHECK(p.cursor == 2 && p.frontier == 3, "insert above the cursor shifts cursor and frontier together");
        CHECK(!p.reseat, "…and needs no re-seat: mpv's held entry is still the one after the playing track");
    }
    // AT the cursor — the new track lands immediately BEFORE what is playing. Same reasoning as above.
    {
        const Plan p = QueueEdit::planInsert(live, 1, 1);
        CHECK(p.cursor == 2 && p.frontier == 3 && !p.reseat, "insert AT the cursor shifts it, mpv unaffected");
    }
    // Appending is `at == count`, and it is valid: the frontier is nowhere near the end of a five-track queue.
    {
        const Plan p = QueueEdit::planInsert(live, 5, 1);
        CHECK(p.valid && p.count == 6 && !p.reseat && p.cursor == 1 && p.frontier == 2,
              "enqueue (insert at count) is valid and disturbs nothing");
    }
    // Multi-entry inserts shift by n, not by one — an album queued at once is one call.
    {
        const Plan p = QueueEdit::planInsert(live, 0, 3);
        CHECK(p.count == 8 && p.cursor == 4 && p.frontier == 5, "insert of n shifts the cursor by n");
    }

    // ---- removing --------------------------------------------------------------------------------------
    // The CURRENTLY PLAYING track. Pinned behaviour: advance onto whatever takes its place. A hard
    // replace-load owns that, which is why the frontier goes back to "nothing handed over" rather than being
    // renumbered. [0,1*,2,3,4] -> [0,2,3,4] with 2 (now index 1) playing.
    {
        const Plan p = QueueEdit::planRemove(live, 1);
        CHECK(p.valid && p.cursorRemoved, "remove: deleting the playing track is reported as such");
        CHECK(p.playIndex == 1, "…and playback advances to the track that took its place");
        CHECK(p.frontier == -1 && !p.reseat, "…with the frontier reseeded by the replace-load, not repaired");
    }
    // The playing track when it is also the LAST one: there is nothing to advance to, so playback stops.
    // -1 here is what makes PlaybackSession emit playbackStopped() rather than advancing or wrapping.
    {
        const Plan p = QueueEdit::planRemove({ 3, 2, 2 }, 2);
        CHECK(p.cursorRemoved && p.playIndex == -1, "remove: deleting the playing LAST track stops playback");
    }
    // A queue of one, playing it: the same answer, and the count really does reach zero.
    {
        const Plan p = QueueEdit::planRemove({ 1, 0, 0 }, 0);
        CHECK(p.valid && p.count == 0 && p.cursorRemoved && p.playIndex == -1,
              "remove: emptying a queue of one stops playback");
    }
    // ABOVE the cursor: cursor and frontier shift down together, mpv's held entry is still the right one.
    {
        const Plan p = QueueEdit::planRemove(live, 0);
        CHECK(p.cursor == 0 && p.frontier == 1 && !p.reseat, "remove above the cursor: no re-seat");
    }
    // The HELD entry itself. mpv is decoding-next a file the app has just deleted; this is the crossing that
    // would play a removed track if it went unreported.
    {
        const Plan p = QueueEdit::planRemove(live, 2);
        CHECK(p.cursor == 1 && p.reseat && p.frontier == 1,
              "removing the entry mpv already holds CROSSES the frontier");
    }
    // Past the frontier: nothing mpv has been handed changed.
    {
        const Plan p = QueueEdit::planRemove(live, 3);
        CHECK(p.cursor == 1 && p.frontier == 2 && !p.reseat, "remove past the frontier leaves mpv alone");
    }
    // Removing the LAST track while playing an earlier one is an ordinary edit, not the stop case.
    {
        const Plan p = QueueEdit::planRemove(live, 4);
        CHECK(p.valid && !p.cursorRemoved && !p.reseat && p.count == 4, "remove the last (idle) track");
    }

    // ---- moving ----------------------------------------------------------------------------------------
    // Playing #2, mpv holds #3, for the crossing cases.
    const State mid { 5, 2, 3 };
    // ACROSS THE CURSOR, UPWARD: [0,1,2*,3,4] -> [0,4,1,2*,3]. The playing track and the entry after it are
    // both displaced by one, TOGETHER, so mpv is still right and nothing is re-seated. This is the case a
    // "reseat whenever the edit touches an index <= frontier" rule would get wrong (over-cautiously).
    {
        const Plan p = QueueEdit::planMove(mid, 4, 1);
        CHECK(p.valid && p.cursor == 3, "move across the cursor upward carries the cursor with it");
        CHECK(p.frontier == 4 && !p.reseat, "…and the held entry follows: no re-seat");
    }
    // ACROSS THE CURSOR, DOWNWARD: [0,1,2*,3,4] -> [1,2*,3,0,4]. Same again, in the other direction.
    {
        const Plan p = QueueEdit::planMove(mid, 0, 3);
        CHECK(p.cursor == 1 && p.frontier == 2 && !p.reseat, "move across the cursor downward: no re-seat");
    }
    // MOVING THE HELD ENTRY AWAY: [0,1,2*,3,4] -> [3,0,1,2*,4]. The app's next is now #4; mpv still holds #3.
    {
        const Plan p = QueueEdit::planMove(mid, 3, 0);
        CHECK(p.cursor == 3 && p.reseat && p.frontier == 3, "moving the held entry away CROSSES the frontier");
    }
    // MOVING SOMETHING INTO THE NEXT SLOT: [0,1,2*,3,4] -> [0,1,2*,4,3]. mpv holds #3, the app now says #4.
    {
        const Plan p = QueueEdit::planMove(mid, 4, 3);
        CHECK(p.cursor == 2 && p.reseat, "moving a track INTO the next slot crosses the frontier");
    }
    // "PLAY NEXT" ON A ROW ABOVE THE CURSOR is a move to `cursor`, not to `cursor + 1`: lifting the row out
    // shifts the cursor down one first. [0,1,2,3*,4] -> [0,2,3*,1,4], and 1 really is the entry after 3.
    {
        const Plan p = QueueEdit::planMove({ 5, 3, 4 }, 1, 3);
        CHECK(p.cursor == 2, "play-next from above the cursor: the cursor shifts down one");
        CHECK(QueueEdit::mapMove(1, 1, 3) == p.cursor + 1, "…and the moved row lands immediately after it");
        CHECK(p.reseat, "…which is a crossing edit: mpv was holding the old next track");
    }

    // ---- the states where there is nothing to disagree with ----------------------------------------------
    // GAPLESS OFF (or a queue never started): the frontier sentinel is -1 and stays -1 — it must never be
    // renumbered into a real index by an edit, because a real index would claim mpv holds something.
    {
        const Plan p = QueueEdit::planInsert({ 5, 1, -1 }, 2, 1);
        CHECK(p.valid && !p.reseat && p.frontier == -1, "gapless off: no frontier, no re-seat, ever");
        // ...and an insert AT OR ABOVE index 0 is the one that would renumber the sentinel if it were run
        // through the mapping like a real index: -1 shifted by an insert at 0 becomes 0, which claims mpv is
        // holding the track that is playing. Asserted separately because the case above (insert at 2) cannot
        // see it — the sentinel is below the insert point there and comes out unchanged either way.
        const Plan top = QueueEdit::planInsert({ 5, 1, -1 }, 0, 1);
        CHECK(top.valid && top.frontier == -1, "gapless off: an insert at the top does not renumber the sentinel");
    }
    // NOTHING PLAYING: the cursor stays -1, no removal can be a removal OF it, and the frontier sentinel is
    // likewise left alone (the same renumbering trap as above — an insert at 0 would turn -1 into 0).
    {
        const Plan ins = QueueEdit::planInsert({ 3, -1, -1 }, 0, 1);
        CHECK(ins.valid && ins.cursor == -1 && !ins.cursorRemoved && !ins.reseat, "edit while not playing: insert");
        CHECK(ins.frontier == -1, "edit while not playing: the frontier sentinel survives the insert");
        const Plan rem = QueueEdit::planRemove({ 3, -1, -1 }, 0);
        CHECK(rem.valid && rem.cursor == -1 && !rem.cursorRemoved, "edit while not playing: remove");
    }
    // A STALE FRONTIER WITH NO CURSOR — the state clearQueue actually leaves behind (it resets trackIndex_ and
    // empties the list, but appendedThrough_ keeps the last queue's value), so an enqueue into a queue that was
    // just cleared arrives here. Without the "no cursor, nothing to disagree with" guard the crossing loop runs
    // against a cursor of -1 and reports a re-seat for a player that is holding nothing at all — which would
    // fire playlist-remove and a re-feed over whatever mpv is doing next.
    {
        const Plan p = QueueEdit::planInsert({ 0, -1, 2 }, 0, 1);
        CHECK(p.valid && !p.reseat && p.frontier == -1,
              "a stale frontier with no cursor (post-clearQueue) never asks for a re-seat");
    }
    // The frontier EQUAL to the cursor is the "mpv holds only what it is playing" state (right after a
    // replace-load, before the one-ahead feed). The window (cursor, frontier] is empty, so nothing crosses.
    {
        const Plan p = QueueEdit::planInsert({ 3, 0, 0 }, 1, 1);
        CHECK(p.valid && !p.reseat && p.frontier == 0, "frontier == cursor: the held window is empty");
    }

    // ---- refusals --------------------------------------------------------------------------------------
    // An out-of-range or no-op edit changes NOTHING, so a caller may ask without pre-checking. A Plan that
    // came back invalid must not carry a usable-looking cursor either.
    CHECK(!QueueEdit::planInsert(live, -1, 1).valid, "refuse: insert before the start");
    CHECK(!QueueEdit::planInsert(live, 6, 1).valid, "refuse: insert past the end");
    CHECK(!QueueEdit::planInsert(live, 2, 0).valid, "refuse: insert of nothing");
    CHECK(!QueueEdit::planRemove(live, 5).valid, "refuse: remove past the end");
    CHECK(!QueueEdit::planRemove(live, -1).valid, "refuse: remove before the start");
    CHECK(!QueueEdit::planRemove({ 0, -1, -1 }, 0).valid, "refuse: remove from an empty queue");
    CHECK(!QueueEdit::planMove(live, 2, 2).valid, "refuse: move onto itself");
    CHECK(!QueueEdit::planMove(live, 5, 0).valid, "refuse: move from past the end");
    CHECK(!QueueEdit::planMove(live, 0, 5).valid, "refuse: move to past the end");

    // ---- the mappings themselves -------------------------------------------------------------------------
    // Pinned directly as well as through the plans, because every plan above reads them and an off-by-one
    // here is the failure that plays the wrong song without anything going red.
    CHECK(QueueEdit::mapInsert(2, 2, 1) == 3, "mapInsert: the entry AT the insert point shifts");
    CHECK(QueueEdit::mapInsert(1, 2, 1) == 1, "mapInsert: an entry above it does not");
    CHECK(QueueEdit::mapRemove(2, 2) == -1, "mapRemove: the removed entry is gone");
    CHECK(QueueEdit::mapRemove(3, 2) == 2 && QueueEdit::mapRemove(1, 2) == 1, "mapRemove: only what is below shifts");
    // [A,B,C,D] with from=0,to=2 is [B,C,A,D] — the moved element at 2, D still at 3.
    CHECK(QueueEdit::mapMove(0, 0, 2) == 2 && QueueEdit::mapMove(1, 0, 2) == 0
          && QueueEdit::mapMove(2, 0, 2) == 1 && QueueEdit::mapMove(3, 0, 2) == 3,
          "mapMove: forward move permutes exactly the span it crosses");
    // ...and from=3,to=1 is [A,D,B,C].
    CHECK(QueueEdit::mapMove(3, 3, 1) == 1 && QueueEdit::mapMove(1, 3, 1) == 2
          && QueueEdit::mapMove(2, 3, 1) == 3 && QueueEdit::mapMove(0, 3, 1) == 0,
          "mapMove: backward move permutes exactly the span it crosses");

    if (fails == 0) printf("QUEUEEDIT-OK\n");
    return fails == 0 ? 0 : 1;
}
