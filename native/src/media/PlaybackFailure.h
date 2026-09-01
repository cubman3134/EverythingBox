// WHAT A FILE THAT WOULD NOT OPEN OWES THE SCREEN (issue #228) — the pure rules, and nothing else.
//
// WHY THIS FILE EXISTS. mpv reports a file it could not open exactly once, as an END_FILE carrying
// MPV_END_FILE_REASON_ERROR, and then goes quiet. It does NOT emit a `pause` property change (it never
// paused — it went idle) and it emits no further positions. Every surface in this app that shows playback
// state is fed by those two signals, so all of them keep the last thing they were told: the now-playing
// page keeps the pause glyph it was given when the open began, and the seek bar and time label keep the
// previous item's timeline. The listener is shown a cover, a running timeline and a "this is playing"
// glyph over silence, and has to press Stop themselves to make the screen agree with the speakers.
//
// So a failure is not just something to SAY. It is a state correction owed to three places at once, and the
// app already knew that on one path and not the other:
//
//   * reportBookPartUnavailable (#217) — "no link existed to open" — stops, zeroes, and shows stopped;
//   * the loadFailed handler — "mpv could not open the link" — said its piece and corrected nothing.
//
// The two differ only in who noticed. They had better not differ in what the listener sees, which is what
// this table is for: one answer, taken by both, that a probe can drive without mpv, a window or an audio
// device.
//
// THE ONE CARVE-OUT, and it is the whole reason this is a table rather than a constant. With gapless armed
// (#141) the app feeds mpv's OWN playlist one entry ahead, so mpv holds two or more files and advances
// between them by itself. A failed entry in that playlist is reported the same way — END_FILE with reason
// ERROR — while mpv carries straight on into the next one. Stopping the player there would silence music
// that is playing correctly, and a sticky message would sit over it forever. That failure owes a timed
// message and nothing else.
//
// NOTHING HERE TOUCHES THE WORLD. No Qt, no player, no settings, no filesystem. Same relationship to the
// host that BackgroundAudio.h has: it produces an answer and stops.
#pragma once

#include "../ui/FeedbackPolicy.h"   // the feedback-duration bands (Qt-free, constexpr only)

namespace PlaybackFailure
{
    // ---- The two facts the rule turns on ---------------------------------------------------------------
    struct Situation
    {
        // Does mpv hold a playlist it will advance by itself? MainWindow::gaplessAudioActive_ — armed only
        // for a gapless AUDIO queue of more than one track, which is the only shape that ever appends to
        // mpv's playlist. Every other load (a film, a stream, an audiobook part, a single track) hands mpv
        // exactly one file, so its failure is the end of everything that was playing.
        bool gaplessArmed = false;

        // Is the themed now-playing audio page the surface? It decides how long the message lives, and only
        // that — see noticeMs below.
        bool themedAudioPage = false;
    };

    // ---- What the failure owes -------------------------------------------------------------------------
    struct Response
    {
        // Stop the player. Not because mpv is still running — it is already idle — but because the app's own
        // notion of "a session is live" is what feeds the menu-music question and the route back to the
        // now-playing page, and both of them are wrong while a dead session is still counted.
        bool stopPlayer = true;

        // Zero the transport: duration, position, the classic seek bar and time label. These are MainWindow's
        // copies of "how long is the thing playing" and "where is it", and with nothing playing the honest
        // value for both is none. Left alone they show the PREVIOUS item's numbers, which is worse than
        // showing nothing because it is legible and false.
        bool zeroTransport = true;

        // Show stopped: themedAudioPaused_ = true, so the page's one live-state glyph reads "play". This is
        // the specific lie the issue was filed about.
        bool showStopped = true;

        // How long the message lives. 0 (kFeedbackSticky) means it stays until something replaces it.
        //
        // STICKY IS FOR THE AUDIO PAGE ONLY, and it is not decoration. Everywhere else a toast is a passing
        // remark made over a screen that still has something on it; on the audio page the whole screen is a
        // cover, 0:00 and a transport that does nothing, so a message that expires leaves someone staring at
        // exactly what they were staring at before with no idea it ever said anything.
        int noticeMs = kFeedbackLong;
    };

    // The table. Note that the gapless arm turns off all three corrections TOGETHER: they are one claim
    // ("nothing is playing"), and that claim is simply false while mpv carries on into the next entry.
    inline Response plan(const Situation& s)
    {
        Response r;
        if (s.gaplessArmed)
        {
            r.stopPlayer = r.zeroTransport = r.showStopped = false;
            r.noticeMs = kFeedbackLong;   // never sticky: it would sit over a track that is playing fine
            return r;
        }
        r.noticeMs = s.themedAudioPage ? kFeedbackSticky : kFeedbackLong;
        return r;
    }
}
