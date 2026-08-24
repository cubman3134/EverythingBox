// EDITING A QUEUE YOU ARE LISTENING TO (issue #193) — the pure index arithmetic, and nothing else.
//
// WHY THIS IS A SEPARATE FILE, and why it owns no state. PlaybackSession offered exactly three verbs
// (setQueue / clearQueue / handleTrackEnd), so the only way to change what was coming up next was to destroy
// what was playing. Adding insert / remove / move to it is three lines of QList surgery and one genuinely
// sharp problem, and the sharp problem is not the list — it is that under gapless (#141, on by DEFAULT since
// a3c3652) the queue is not just `tracks_` and `trackIndex_`. mpv holds its OWN playlist, fed one entry
// ahead, and an edit that lands on an entry mpv has ALREADY been handed makes the two silently disagree: mpv
// plays the old track, the app announces the new one, and the symptom is the wrong song rather than a crash.
//
// So the decision an edit really has to make is "did this cross the append frontier", and that decision is
// pure arithmetic over four integers. It lives here, where a probe can drive every edge of it without mpv, a
// window, or an audio device — the same relationship MusicQueue has to PlaybackSession, and the same one
// PlaybackSession::tracksCompleted already has to the gapless boundary.
//
// THE MODEL, stated once so the rules below are checkable rather than asserted:
//
//   * `cursor`   = PlaybackSession::trackIndex_, the queue index now playing (-1 = nothing playing).
//   * `frontier` = PlaybackSession::appendedThrough_, the HIGHEST queue index handed to mpv's own playlist.
//                  -1 means nothing has been handed over at all (gapless off, or a queue never started), and
//                  is the whole of "this edit cannot possibly disagree with mpv".
//   * mpv therefore holds, un-played and already committed, exactly the queue indices in (cursor, frontier].
//     maybeAppendNext's one-ahead invariant keeps that window at zero or one entry, but nothing here assumes
//     it: the rules are written for a window of any width so the arithmetic stays correct if the feed ever
//     runs deeper.
//   * `prevPos_` is deliberately ABSENT from this input set, and that is a claim, not an oversight. It is
//     mpv's playlist position, not a queue index, and the reseat the host performs for a crossing edit only
//     ever drops entries AFTER mpv's current position — so the position of the entry that is playing never
//     moves, and the next boundary still arrives as prevPos_ + 1. An edit that removes the PLAYING track is
//     the one case that cannot be repaired that way, and it is answered with a hard replace-load, which
//     reseeds prevPos_ and the frontier together the way playIndex always has.
//
// THE CHOICE THIS FILE ENCODES (the brief's (a) vs (b)). It is (a) — RE-SEAT mpv's playlist when an edit
// crosses the frontier — and (b) is not merely worse here, it is unimplementable. (b) says "keep edits
// strictly above the frontier"; with gapless on by default the frontier is cursor + 1, and cursor + 1 is
// exactly where "play next" has to insert. A rule that forbids the crossing forbids the feature. What (a)
// costs is one `playlist-remove` of an entry mpv has not started decoding, seconds before it would have been
// needed, followed by the same one-ahead append that would have happened anyway — inaudible by construction,
// because the only entries dropped are ones no sample has been taken from. So `reseat` below is not an error
// flag: it is the plan telling the host "hand mpv its next entry again".
//
// CONSERVATIVE ON PURPOSE. The crossing test compares, entry by entry, where each already-handed index ENDS
// UP against where the cursor's successor chain says it should be. An edit that permutes the window without
// changing what follows the cursor (a move entirely below the cursor, an insert past the frontier) reports no
// crossing and mpv is left alone. Anything else reports one. A false positive costs a re-append; a false
// negative plays the wrong song.
#pragma once

namespace QueueEdit
{
    // The four numbers an edit reads. Nothing here is a pointer into the track list, so a probe can state a
    // case in one line and an assertion can be written against a hand-computed answer.
    struct State
    {
        int count = 0;      // tracks_.size()
        int cursor = -1;    // trackIndex_: the queue index playing now, or -1
        int frontier = -1;  // appendedThrough_: highest index handed to mpv, or -1 for "nothing handed over"
    };

    // What the edit does to those numbers, and what the host owes mpv afterwards.
    struct Plan
    {
        bool valid = false;         // false = the edit is out of range / a no-op; the caller changes nothing
        int  count = 0;             // tracks_.size() after the edit
        int  cursor = -1;           // trackIndex_ after the edit
        int  frontier = -1;         // appendedThrough_ after the edit
        bool reseat = false;        // mpv holds an entry this edit invalidated: drop past-current, re-feed
        bool cursorRemoved = false; // the edit removed the track that was PLAYING
        // Only meaningful when cursorRemoved. >= 0 = the queue index to (re)start with a hard replace-load
        // (the track that took the removed one's place). -1 = there was nothing after it, so playback stops.
        int  playIndex = -1;
    };

    // ---- where one old index ends up after each edit (-1 = the entry is gone) --------------------------
    // Written as three tiny functions rather than inline in the planners because the crossing test and the
    // cursor both need the SAME mapping, and two hand-written copies of "an insert shifts everything at or
    // after `at`" is how one of them ends up off by one in the direction that plays the wrong song.
    inline int mapInsert(int i, int at, int n) { return i >= at ? i + n : i; }
    inline int mapRemove(int i, int at) { return i == at ? -1 : (i > at ? i - 1 : i); }
    inline int mapMove(int i, int from, int to)
    {
        if (i == from) return to;
        const int j = i > from ? i - 1 : i;   // index after the element is lifted out
        return j >= to ? j + 1 : j;           // ...and after it is dropped back in at `to`
    }

    // The shared tail of all three planners: carry the cursor across the edit, then ask whether any entry mpv
    // already holds still sits where the cursor's successor chain expects it. `mapped` is one of the three
    // above with its parameters bound.
    template <typename Map>
    inline Plan planWith(const State& s, int newCount, Map mapped)
    {
        Plan p;
        p.valid = true;
        p.count = newCount;
        p.cursor = s.cursor < 0 ? -1 : mapped(s.cursor);
        p.cursorRemoved = s.cursor >= 0 && p.cursor < 0;
        if (p.cursorRemoved)
        {
            // A hard replace-load reseeds prevPos_ and the frontier the way it does for every manual jump, so
            // there is nothing here to carry across: the successor takes the removed track's index, and if
            // there is no successor the queue has nothing left to play.
            p.playIndex = s.cursor < newCount ? s.cursor : -1;
            p.cursor = p.playIndex;
            p.frontier = -1;
            p.reseat = false;
            return p;
        }
        if (s.frontier < 0 || s.cursor < 0)
        {
            // Nothing was ever handed to mpv (gapless off, or no queue running): there is nothing to disagree
            // with, and the frontier stays the "nothing handed over" sentinel rather than being renumbered
            // into a real index by accident.
            //
            // The `s.cursor < 0` half is the one with teeth, and it is not hypothetical: clearQueue resets
            // trackIndex_ and empties the list but LEAVES appendedThrough_ at the departed queue's value, so
            // an enqueue into a just-cleared queue arrives here with a real frontier and no cursor. Without
            // this the crossing loop would run against a cursor of -1, report a re-seat, and fire
            // playlist-remove at a player that is holding nothing. probe_queueedit pins that case by name.
            p.frontier = -1;
            p.reseat = false;
            return p;
        }
        bool broke = false;
        for (int i = s.cursor + 1; i <= s.frontier; ++i)
        {
            const int ni = mapped(i);
            // The entry mpv holds `k` slots after the playing one must still be `k` slots after it. Anything
            // else — removed, displaced by an insert, permuted by a move — means mpv is holding a file the
            // app no longer believes comes next.
            //
            // A REMOVED entry (ni == -1) needs no clause of its own, and there deliberately is not one: the
            // cursor is >= 0 here (the cursor-removed case returned above) and i > s.cursor, so the expected
            // value is at least p.cursor + 1, which -1 can never equal. An explicit `ni < 0 ||` was written
            // first and mutation testing showed it inert — deleting it changed no verdict, because this
            // comparison already catches it. It is said here instead of guarded for, so the next reader does
            // not add it back.
            if (ni != p.cursor + (i - s.cursor)) { broke = true; break; }
        }
        p.reseat = broke;
        // A crossing edit shrinks the frontier back to the playing entry: after the host drops mpv's
        // past-current entries, the playing one is again the highest index mpv has been handed, which is
        // exactly the state maybeAppendNext's one-ahead invariant expects before it feeds again.
        p.frontier = broke ? p.cursor : mapped(s.frontier);
        return p;
    }

    // Insert `n` entries so the first lands at index `at`. `at == count` appends.
    inline Plan planInsert(const State& s, int at, int n)
    {
        if (n <= 0 || at < 0 || at > s.count) return Plan();
        return planWith(s, s.count + n, [at, n](int i) { return mapInsert(i, at, n); });
    }

    // Drop the entry at `at`.
    inline Plan planRemove(const State& s, int at)
    {
        if (at < 0 || at >= s.count) return Plan();
        return planWith(s, s.count - 1, [at](int i) { return mapRemove(i, at); });
    }

    // Move the entry at `from` so it ends up at index `to`.
    inline Plan planMove(const State& s, int from, int to)
    {
        if (from < 0 || from >= s.count || to < 0 || to >= s.count || from == to) return Plan();
        return planWith(s, s.count, [from, to](int i) { return mapMove(i, from, to); });
    }
}
