# A stream that stalls is reported, not waited on forever

**Date:** 2026-09-01
**Follows:** 2c7cc246 (`Merge: a failed open stops the transport instead of leaving it reading "playing"`)
**Closes:** #213

## The problem

#228 fixed what happens when mpv *refuses* a file: the transport is stopped, zeroed and the reason
shown. That path is reached from exactly one event — `END_FILE` with reason `ERROR` — which mpv
emits only when it gives up.

A source that is dead or throttled does not make mpv give up. The connection opens, mpv emits
`START_FILE`, and then nothing: no `FILE_LOADED`, no `END_FILE`, no positions. The page shows a
cover, `0:00`, and a dead transport, and the app has nothing to say because as far as it knows the
load is still in progress. #213's report is the log of exactly that: `open: … type=audiobook` and
then silence.

So the existing handling covers "mpv said it failed" and not "mpv said nothing at all", and the
second is the commoner shape when a source is degraded rather than gone.

## What we are building

A **load watchdog** in `MpvWidget` that turns that silence into a failure, routed through the
correction #228 built so the transport, the sticky-on-the-audio-page rule and the gapless carve-out
all apply unchanged — with its own wording, because the remedy is different.

### Detection: two phases, and the question asked is "did any byte arrive"

The issue's own guidance is followed: prefer *no bytes and no file-loaded event at all* over a
wall-clock guess about speed.

```
play(url) ── watches(url)? ── no ──▶ nothing armed (live/HLS untouched)
                │ yes
          arm phase 1 (12 s), on START_FILE
                │
     ┌──────────┼─────────────────────────┐
FILE_LOADED   END_FILE                timer fires
  disarm        disarm                    │
  play          existing paths         mpv says?
                                    ┌─────┴─────┐
                                  none      some/unknown
                                STALL       re-arm once (20 s)
                                                │
                                          still no FILE_LOADED → STALL
```

* **Phase 1, 12 s.** At the deadline mpv is asked what it can say about the file's bytes
  (`demuxer-cache-time`, `stream-pos`). The answer is **three-valued**, and the middle value was
  found live: *some* → slow but alive → one re-grace; *unknown* — no demuxer-level property answers
  at all, which is what a link still warming up looks like, because until enough bytes arrive to
  identify the format there is no demuxer → also re-grace; *none* — a demuxer answered and reports
  an empty cache → positive evidence of death → stall now. Reading *unknown* as *none* is the
  slow-link kill this design exists to avoid, and it was the first cut's bug: a server that sent
  headers plus 4 KiB then hung was indistinguishable from one that sent nothing.
* **Phase 2, 20 s more.** If `FILE_LOADED` still has not come, it is a stall regardless of bytes:
  this is the "buffered a little and then died" shape, and a single no-progress deadline would let it
  hang forever. Phase 2 never re-arms, so a load that never progresses always terminates, at 32 s.

Both deadlines are named constants in `LoadWatchdog.h`, not settings. Half a minute is long enough
that a cold debrid link is not killed; a tighter value is a one-line change if it proves so.

### Scope: every load except live/HLS

Audiobooks, music, films and on-demand streams all stall identically, so all are watched. Local files
never stall and are watched harmlessly. `.m3u8` / `.m3u` links are **excluded** — the same test the
app already uses to classify HLS at `MainWindow.cpp:1485`. A live channel buffers differently, a
channel merely slow to open would be turned into an error message, and IPTV has its own skip
handling. A VOD `.m3u8` is excluded too; that is the conservative direction, which is the right one
for a watchdog.

Only the **active deck** is watched. The crossfade deck's failure has its own path in
`handleEvent`'s `!fromActive` branch, and a promoted deck is already past `FILE_LOADED`. A gapless
advance is a fresh `START_FILE` on the active deck and re-arms naturally; its stall reaches the host,
whose `PlaybackFailure::plan` already answers "timed message, no stop" for gapless.

### Response: stop and say so, in its own words

`MpvWidget` emits `loadStalled(waitedSeconds)`. It does **not** stop mpv itself: what the screen is
owed is the host's decision (`PlaybackFailure::plan`), and the host's `showPlaybackStopped` is what
stops the player. Doing it in the widget would pre-empt the gapless carve-out.

In `MainWindow` the tail of the `loadFailed` lambda — plan → `showPlaybackStopped` → sticky-notice
ownership — becomes `reportOpenFailure(message)`, and both the failure and the stall lambdas build
their own sentence and hand it there. One correction, two reasons. The stall wording says the source
sent nothing for N seconds and suggests trying again or choosing another source from the shelf; it
does **not** say the link expired, because it usually has not, and "mint a fresh one" is the wrong
remedy.

## Components

| Unit | Owns | Depends on |
|---|---|---|
| `native/src/media/LoadWatchdog.h` | `watches(url)`, `judge(tick) → Loaded/Regrace/Stall`, the two deadlines, `waitedSeconds(phase)`. Pure: no Qt, no mpv. | nothing |
| `MpvWidget` | the `QTimer`, arming on `START_FILE`, disarming on `FILE_LOADED`/`END_FILE`/`stop()`, the mpv progress query, `loadStalled` | `LoadWatchdog.h`, libmpv |
| `MainWindow::reportOpenFailure` | plan → `showPlaybackStopped` → ownership record; the door both failure shapes go through | `PlaybackFailure.h` (#228) |

## Testing

* **`probe_loadwatchdog`** over `LoadWatchdog.h`: the live exclusion (`.m3u8`, `.M3U8`, with a query
  string, `.m3u`; a path that merely *contains* `m3u8` is watched), every `judge` arm, that phase 2
  never re-graces (termination), and that `waitedSeconds` reports the total. Registered in all three
  places; mutation-checked before it is trusted.
* **The #217/#228 source gate** is extended: `reportOpenFailure` exists and reaches
  `showPlaybackStopped`, and *both* the `loadFailed` and `loadStalled` handlers route through it.
  A third failure shape that picked up the message half and forgot the correction half — which is
  exactly how #228 happened — fails the gate.
* **Live:** the progress properties are the one thing a headless probe cannot check. The watchdog logs
  which property answered, so one real stall on the rig confirms the phase-1 question is being asked
  of a populated value rather than an absent one. It was not, at first: the first cut read an absent
  property as "no bytes", and a dead source and a trickling one both stalled at 12 s. Hence the
  three-valued answer above, after which the trickling source re-graces and stalls at 32 s.

## Not in scope

* Auto-trying the next source on a stall. It would start a second multi-second resolve after the
  listener has already waited out the timeout, and can silently substitute a different release.
* A user-visible timeout setting.
* The unexplained title/query mismatch noted at the bottom of #213 (asked for one book, searched for
  another). Separate, and worth confirming on its own.
