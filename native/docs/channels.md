# Channels

A **channel** turns part of your library into something you tune to rather than browse. It has a lineup, it
runs to a clock, and when you switch to it you join whatever is on **in progress** — you land at 00:12:34
because that is where the clock is, the way a television always worked.

This page describes what shipped in the first increment of issue #179. What is *not* here yet is listed at
the bottom, and it is a real list — read it before wondering why something is missing.

## What a channel is

Three things, and nothing else:

| | |
|---|---|
| **A source** | Where its programmes come from: a saved video **playlist**, or a **local series**. |
| **An ordering** | **In order** (the source's own order) or **Shuffle**. |
| **A start epoch** | The moment it went on air. Before that moment the channel simply has no programmes — a channel you make this afternoon does not claim to have been broadcasting all morning. |

Plus a name, and one switch: **Start programmes from the beginning**, for people who would rather not join
halfway through.

Channels are stored per profile and sync between your devices with the rest of your small stores
(favourites, playlists, saved filters). Deleting one leaves a dated tombstone, so another device that still
has a copy cannot quietly put it back.

## Where they are

**Video → Channels.** The folder is always there; its last row, *Create a channel…*, is how you make the
first one. Activating a channel row **tunes** it. Long-press (or the context key) on a channel row to
**edit or delete** it.

A channel is an ordinary item: you can favourite it, and tuning one puts *the channel* in Continue Watching —
not the episode it happened to be showing. Re-opening that row tunes the channel again and lands you wherever
the clock has got to, which is the only re-open that makes sense for something that has been running without
you.

Other parts of the app can address a channel by the key `channel:<id>`.

## How the lineup is computed

The schedule is a **pure function** of the channel, the day, and its items' lengths. It is computed, never
stored — which is what lets every device work out the same answer without asking each other.

1. **The day is the unit.** A schedule covers one local day, from midnight to midnight.
2. **The order is seeded by the channel and the day.** Shuffle is a seeded shuffle — `hash(channel id, day)` —
   so your phone, your TV and your desktop all draw the *same* running order for the same day. It is not the
   system random number generator, which would give a different answer on every device and, in fact, on every
   restart.
3. **Programmes are laid end to end** from when the channel went on air until the day is full, repeating the
   lineup as often as it takes.
4. **What is on now** is whichever programme's window contains the current second, and how far into it you
   are is the difference. That offset is what tuning seeks to — unless the channel is set to start programmes
   from the beginning.

### Today's lineup does not move

Once a day's lineup has been computed it is **frozen** for the rest of that day. Add an episode to the source
at 20:15 and the evening you are watching does not re-cut itself under you; the new episode is in tomorrow's
lineup. A guide that changed its mind halfway through the evening would be worse than no guide.

### An item needs a known length

A programme can only be scheduled if the app already **knows how long it is**. Lengths come from the duration
index, which records the runtime of anything you have played; nothing is opened or probed while a lineup is
being built, because a channel that stalls at every programme boundary is worse than no channel at all.

Items with no known length are **skipped** — quietly, and named once in the log. In practice this means a
brand-new file joins its channel after it has been played once. The duration index is device-local: it is not
synced, because the other device works the number out for itself the first time it plays anything.

## Surfing

While a channel is playing, **Up** and **Down** change channel. Each switch resolves what is on *that*
channel right now and joins it, so surfing lands you mid-programme exactly as the first tune did. A brief
banner names the channel, what is on, how long is left and what is next.

The two channels either side of the one you are watching have their day computed and cached in advance —
bounded to exactly those two, so surfing stays instant without the app quietly doing work for a hundred
channels you are not watching.

Playing anything else, or going Home, stops the channel.

### "time remaining" on a start-from-the-beginning channel

On an ordinary channel you joined mid-programme, so the schedule's remaining time is your remaining time. On
a channel set to **start programmes from the beginning** you are watching the whole thing from zero, so the
banner shows the programme's own length instead — the schedule's remaining would be a number about a
broadcast nobody in the room is watching. The consequence is real and intended: such a channel runs *behind*
its own schedule, because each programme is watched in full before the next one is resolved.

Starting from the beginning also overrides any position you left in that file. It means what it says.

## Not here yet

* **The guide.** A channels × time grid is the next increment; it shares the programme model the XMLTV
  guide (#75) already uses, so both feed one grid.
* **Interstitials.** Bumpers and idents between programmes.
* **Time-blocked ordering** ("this block from 20:00").
* **Saved-filter, addon-catalogue and server sources.** A saved filter (#63) can be *stored* as a channel's
  source and survives a sync, but nothing enumerates it yet: a saved filter is a game-library filter, and
  answering "which items match it" needs a whole-library enumeration this app builds one browse level at a
  time. The editor therefore does not offer it. Addon catalogues and server items are the same seam.
* **Streams.** Channels are built from items that can be enumerated with known lengths. A debrid or torrent
  stream has neither, and resolving one per programme is neither fast nor free.
