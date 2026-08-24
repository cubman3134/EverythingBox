// MUSIC THAT SURVIVES LEAVING ITS PAGE (issue #193, increment 3) — the pure rules, and nothing else.
//
// WHY THIS FILE EXISTS. Until this increment, Back on the now-playing page ran
// `player_->stop(); session_->clearQueue();` on BOTH layouts (MainWindow::leaveThemedAudioPage, and
// goBack()'s content-page branch). The old comment beside it was honest — it "PRESERVES today's classic
// behaviour EXACTLY" — and said nothing about whether that behaviour was right. It was not: it meant the app
// could not play an album while you looked at anything else, so every feature the music arc built on top of a
// queue (multi-album queues, gapless, ReplayGain, crossfade, and both queue-editing increments) was reachable
// only from a page you could not leave without silencing it. Increment 2's `Append` arm had no live path AT
// ALL for exactly this reason.
//
// The change is small to write and easy to get subtly wrong, because "leaving the page" was doing FOUR jobs
// at once and only one of them is the page's:
//
//   1. tearing down PAGE state      (the 1 Hz progress throttle, the QML view flip)   — always correct;
//   2. tearing down SESSION state   (the lyric cache, keyed by the track's path)      — only if it ended;
//   3. stopping the player;
//   4. clearing the queue, which is where resume + consumption bookkeeping is FLUSHED (clearQueue calls
//      persistResume, and queueCleared disarms gapless/crossfade).
//
// Which of those four a leave owes depends on ONE question — "is this a live audio session?" — and getting it
// wrong is silent in both directions: keep a video's queue and mpv plays a film's audio behind the home
// screen; drop an audio queue and this increment did nothing. So the answer is a table, here, where a probe
// can drive every arm of it without mpv, a window or an audio device. Same relationship to the host that
// QueueEdit.h and LaunchCancel have: it produces an answer and stops.
//
// NOTHING HERE TOUCHES THE WORLD. No Qt, no player, no settings, no filesystem.
#pragma once

namespace BackgroundAudio
{
    // ---- The two facts every rule below turns on ------------------------------------------------------
    //
    // `videoLatch` IS A LATCH, and that is why it is named one. PlaybackSession::mediaIsVideo_ is written by
    // the host at each open (setMediaVideo) and NOTHING clears it on stop, so an empty queue left behind by a
    // film still reads `video`. Increment 2 hit the same trap from the other side and answered it by asking
    // the COUNT first; the same discipline is kept here, and audioLive() is written so the count alone
    // decides the empty case. A stale latch over an empty queue must never be able to say "there is music
    // playing" — that would put a route back to a track that ended on screen.
    struct Session
    {
        int  queueCount = 0;      // PlaybackSession::count(): 0 == there is no queue at all
        bool videoLatch = true;   // PlaybackSession::mediaIsVideo(): the loaded file's kind. See above.
    };

    // Is there a live AUDIO session — something whose sound is worth keeping when its page closes?
    // Deliberately NOT "is it un-paused": a paused track is still the thing you are listening to, and menu
    // music starting over a pause (then ducking again on resume) is worse than either state.
    inline bool audioLive(const Session& s)
    {
        if (s.queueCount <= 0) return false;   // no queue: the latch says nothing, whatever it still holds
        return !s.videoLatch;
    }

    // ---- Leaving the now-playing page -----------------------------------------------------------------
    //
    // The four jobs, decided together. The default-constructed value is TODAY'S behaviour (stop, clear, drop
    // everything, offer nothing), which is what a caller that cannot use the audio arm — goBack() reached
    // from a page that is not the player — should take without asking.
    struct Exit
    {
        bool stopPlayer = true;   // player_->stop()
        bool clearQueue = true;   // session_->clearQueue(): flushes resume + consumption, disarms gapless
        bool keepLyrics = false;  // trackLyricsPath_ is SESSION state (keyed by the track's path), not page
                                  // state: clearing it under a track that is still playing costs a re-parse
                                  // and, for a track with no sidecar, a fresh network lyric lookup.
        bool background = false;  // the music plays on with no page: offer the route back to it
    };

    inline Exit planExit(const Session& s)
    {
        if (!audioLive(s)) return Exit{};          // video, IPTV, or nothing playing: exit exactly as before
        Exit e;
        e.stopPlayer = false;
        e.clearQueue = false;                      // …so resume/consumption are flushed at the REAL end of
                                                   // the media instead of at a page exit that is not one
        e.keepLyrics = true;
        e.background = true;
        return e;
    }

    // ---- Something else opening on top of it (increment 4) --------------------------------------------
    //
    // Leaving a page was never the only way the music died. OPENING something else ran the very same
    // `player_->stop(); session_->clearQueue();` at nine call sites of its own, which is why a book still
    // silenced an album after increment 3 taught every *exit* to keep it — and that inconsistency is worse
    // than the original behaviour, because the user has just been taught that music survives browsing.
    //
    // The question a takeover asks is NOT "which page is arriving". It is what the arriving surface OWNS:
    //
    //   * OwnsSound  — a film, a game, an emulator, a cast handoff, another queue. These own the speakers as
    //                  well as the screen, and an album left running under a film is two soundtracks at once.
    //                  Their answer is the pre-#193 plan, unchanged, forever.
    //   * SilentPage — a book, a PDF, a comic, a page of images. These own the SCREEN and nothing else.
    //                  Reading with music on is one of the most ordinary things anyone does with an app like
    //                  this, and there is no sound of the reader's for the music to collide with.
    //
    // Written as ONE function taking the kind, rather than as a rule at each call site, so a reader open and
    // a game launch are answered from the same table — and so "a reader shares the sound" becomes a sentence
    // a probe can read, instead of the ABSENCE of a stop call in nine places, which is invisible to everything.
    enum class Takeover
    {
        OwnsSound,   // it has sound of its own
        SilentPage   // it takes the screen and nothing else
    };

    inline Exit planTakeover(const Session& s, Takeover t)
    {
        if (t == Takeover::OwnsSound) return Exit{};   // stop and clear, exactly as it always did
        return planExit(s);                            // …otherwise the SAME table a page exit takes
    }

    // ---- The route back -------------------------------------------------------------------------------
    //
    // Offered exactly when the music is playing AND its page is closed. Not while the page is up: the route
    // back to a page you are standing on is a row that does nothing, and a menu full of rows that do nothing
    // is how a user learns to stop opening the menu.
    inline bool offerResume(const Session& s, bool pageVisible)
    {
        return audioLive(s) && !pageVisible;
    }

    // Which surface "get me back to it" reopens. `wasThemed` is remembered by the host when the page was
    // OPENED, not derived at resume time: a theme without a `nowplayingAudio` view falls back to the classic
    // player page for the whole session, and re-deriving it from the current layout would send that session
    // back to a page its theme cannot draw.
    //
    // `Nothing` is not a defensive nicety. The affordance is drawn from a menu that was built earlier; the
    // queue can end (played out, or another play took over) between the menu being built and a row being
    // picked, and reopening a now-playing page over an empty session is a blank page with no way to explain
    // itself.
    enum class Reopen { Nothing, ThemedPage, ClassicPlayerPage };

    inline Reopen planReopen(const Session& s, bool wasThemed)
    {
        if (!audioLive(s)) return Reopen::Nothing;
        return wasThemed ? Reopen::ThemedPage : Reopen::ClassicPlayerPage;
    }
}
