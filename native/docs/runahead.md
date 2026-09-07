# Runahead — the response arrives on the frame the button was pressed

Many classic games have **internal input lag**. The game reads the controller on frame *t*, then takes another
one to three frames to draw what happened. That delay is in the game's own code, so no amount of fast video or
low-latency audio removes it: on real hardware it was there too, and everybody just got used to it.

Runahead removes it. Every displayed frame, the emulator advances the game one frame as usual — and then runs
it a few more frames *in advance*, using the input you are holding **right now**, shows you that picture, and
rewinds the emulation back to where it really is. What you see is the frame the game is going to draw, so a
button press appears to take effect immediately.

It is **not** fast-forward. Exactly one frame of real time passes per displayed frame, so the game runs at its
normal speed and the music plays at its normal pitch.

## Turning it on

- **Per game (the one that matters):** pause the game and use the **Runahead** row on the pause menu. It cycles
  Off → 1 frame → 2 frames → 3 frames → Off.
- **Default for every game:** Settings → **Runahead (default)**, on both the themed and the classic settings
  screens.

The default is **Off**, everywhere. Turning it off for a game *removes* that game's setting, so the game goes
back to following the default rather than being permanently pinned to zero.

### Why the setting is per game

Because the correct number **is a property of the game** — its own internal lag — not of your TV, your
controller or your machine. Two games on the same console, running on the same core, routinely want different
numbers. A device-wide value would be wrong for most of your library.

Start at 1 and go up only while the game still feels better. Setting more frames than the game's actual lag
does not make it snappier; it just costs more.

## What it costs

One displayed frame becomes **N+1 core runs, one save state and one restore**. At 2 frames of runahead the
emulator is doing three times the emulation work plus two state operations, sixty times a second.

That is affordable for the 8- and 16-bit systems where runahead matters most: their cores run a frame in a
fraction of a millisecond and their save states are tens of kilobytes. It is not affordable for a heavy 3D
core, and it never will be.

## When it refuses (and why it never gives you slow motion)

A latency feature that halves your frame rate has made the game worse. So runahead measures first and then
either engages or **refuses out loud** — it never quietly delivers a slower game.

At the start of a session it times the first half-second of real frames, then times one save-and-restore round
trip (which leaves the game exactly where it was). If N+1 runs plus those two state operations do not fit
comfortably inside one frame at full speed, runahead does not engage and says so.

It also refuses when:

| Situation | Why |
|---|---|
| The core cannot save states at all | There is nothing to rewind the speculation back to. |
| The core declares unreliable save states | A core can tell the frontend its states are incomplete, need initialising, or change size mid-session. Rolling back onto one of those corrupts the game rather than merely looking wrong. |
| A save or restore actually fails while playing | Runahead switches off for the rest of the session instead of gambling every frame. |
| **Netplay** is running | Lockstep netplay owns the timeline. A player speculating on top of it would desync the session, not just feel different. |
| **Split screen** | Same exclusion rewind and fast-forward already take. |

Every one of those tells you, in a sentence, what happened.

## How it works with everything else

- **Rewind** keeps recording *displayed* frames only. The snapshot for a frame is taken before the sequence
  starts, from the real timeline — so rewinding never drops you into a speculative timeline that never
  happened, and the rewind buffer is identical to what it would hold with runahead off.
- **Achievements** are evaluated on the shown frame only. A hidden run never reaches RetroAchievements, so
  nothing can unlock against a timeline that is about to be discarded.
- **Audio** comes from the shown run only. The hidden runs are muted, which is why runahead does not multiply
  the sample rate.
- **Fast-forward** and **rewind** suspend runahead while they are held — speculating on top of either is
  meaningless.
- **Cheats**, including Cheat Search's address freezes, apply on every run: a freeze is part of the emulation,
  not a spectator of it, and skipping it would show you the un-frozen value.

## For the curious

The ordering is RetroArch's single-instance `runahead_run()` — run once and save, run N-1 more, show the last
one, restore — and it lives in `native/src/emu/Runahead.h` as pure data plus a template, deliberately separate
from the emulator, so `probe_runahead` can drive it against a fake serializable core in CI and assert that a
hundred frames with runahead land on exactly the state a hundred plain frames land on.

A **second-instance** mode (a second copy of the core doing the speculation, which gives cleaner audio at
double the memory) is a possible follow-up. Only single-instance mode ships today.
