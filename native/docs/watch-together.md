# Watch together

Two EverythingBoxes, a five-character room code, and one film. Play, pause and seek stay in step, so
the pause for a cup of tea is everyone's pause and the person who arrives twenty minutes late lands
where the rest of you already are.

It is EverythingBox to EverythingBox. On the same network it needs nothing at all; over the
internet it uses the same relay online netplay already uses.

## Starting one

**"Watch together…" on a film's page**, on both layouts: a pill in the themed detail view's action row
and a button beside Play on the classic detail page, wherever that page offers Play on something that plays
as video. It opens the room menu with that film already chosen:

- **Host a room for "…"** opens the room and starts the film here. A host that starts something shares it,
  so the film is the room's the moment it loads, and whoever joins lands where you are in it.
- **Play this for everyone** is what the same action says, and does straight away, while you are hosting a
  room: the film you are looking at replaces whatever the room was watching, for everyone.

A guest's copy of the action opens the ordinary room menu (Who's watching, Leave), because what the room
plays is the host's choice.

The same menu, without a film chosen, is at **Settings ▸ General ▸ Watch together ▸ "Watch together…"** on
both layouts:

- **Host a room** mints a code — five characters from the same alphabet netplay's rooms use, with
  no ambiguous `0`/`O` or `1`/`I`, because these get read aloud. You are then asked whether the
  other person is on this network or somewhere else.
- **Join with a code…** takes the code and, for a room on the same network, the host's address.

Once you are in a room the same menu offers **Who's watching**, **Play this for everyone** (host
only) and **Leave the room**. During playback the same menu is on **W** — Settings is not reachable
without ending the film, and a film is exactly when the participant list is worth having.

Start playing something and the host's room follows it. If you host while something is already
playing, the guest is handed it the moment they arrive: they get *what* is playing and *where you
are in it* in one go, so a join in progress needs no second press from either of you.

## Seeing the room

While you are in a room the player says so, in the transport row beside the time: how many of you are
watching and, when it matters, what is holding things up.

| The room | What the indicator says |
| --- | --- |
| nobody has joined yet | Just you — room K7Q2M |
| two of you, in step | 2 watching |
| three, one stalled, "wait for everyone" | 3 watching · 1 is buffering · Waiting for Sam to catch up |
| the same, "keep going" | 3 watching · 1 is buffering |
| your own stream stalled | … · Waiting for you to catch up |
| one could not get the film | 2 watching · 1 couldn't play it |

The name appears only when the room is actually *waiting*. Under "keep going" the film carries on, so the
badge says somebody is behind without implying that everyone is holding for them. Somebody who could not
play the film is not counted as buffering, for the reason they cannot hold the room: they are not watching
it.

The indicator sits in the transport row, so it comes and goes with the rest of the controls. A room that
*starts* stalling brings the controls up by itself, since that is exactly when someone wants to know why the
picture stopped. Themed video plays under the same controls; on the themed audio now-playing page the same
line sits under the track status. Both surfaces paint one summary (`WatchTogether::indicatorSummary`), so
they cannot word the room differently. **W** still opens the full menu, with each person's state.

A guest's indicator follows the **host's** stall policy, not its own setting. The host announces it on
every transport and position message (below), so a guest names whoever the room is waiting for only when
the host really is waiting, and a change the host makes mid-film reaches every guest within a second. A
host running a build from before the announcement says nothing; its guests then assume "wait for
everyone", which is the default, and **Who's watching** marks that line as an assumption.

## What actually crosses the wire

**An identity, never bytes, and never a link.**

The room carries a *reference*: a catalogue id (`tt1375666`), an add-on stream reference, or a path
in a local library — the same reference type ["Play on device"](play-on-device.md) hands between
boxes. Each participant then **resolves their own stream**, with their own add-ons and their own
accounts.

This is the whole design, and it is not only about bandwidth:

> A resolved stream URL is a credential. It is signed, it is account-bearing, and on a debrid
> service it *is* your token. It must never go on the wire to somebody else, never be written into
> a log, and never be stored in a room record.

So it is not. The encoder that writes every message scrubs each field it emits: hand it a resolved
URL where the add-on id belongs and what comes out is an empty field, not a leak. `probe_watchtogether`
byte-scans a whole session's transcript — every byte either side would have written to the socket —
and the persisted room record, for a token and for any URL of any scheme, after a drive in which the
host was deliberately handed a signed stream link as the thing to share.

The practical consequence: **you both need your own way to get the film.** Two people with the same
add-ons or the same debrid habit will both resolve it; someone with neither will not, and the room
handles that (below) rather than pretending otherwise.

The traffic is a control channel — roughly ten messages a minute plus one small position beacon a
second. That is why the same Python relay netplay uses carries it without noticing.

The host's transport and position messages also carry its stall policy (`"policy": "wait"` or
`"keepgoing"`). The field is optional in both directions. An older host leaves it out, and its guests read
that as "not told". An older guest ignores a key it does not know. Neither side refuses the other, so the
protocol version did not change.

## Who decides

**The host's transport is authoritative.** Either of you can press pause or seek; a guest's press is a
*request*. It takes effect on the guest's own screen at once — a transport button that waits for a round
trip feels broken — and the host's answer confirms it a moment later. If the host refuses, the guest snaps
back to what the room is doing within a second and a half. That is SyncPlay's model and it is what keeps
one film playing rather than two that agree for a while.

Two rules a host arbitration needs and this one has:

- A seek to an impossible position is clamped by the host, not relayed.
- Somebody who could not get the film **does not get to pause it** for the people who did.

## Staying in step

A guest gets the host's position once a second and corrects gently before it corrects hard:

| Drift | What happens |
| --- | --- |
| up to 0.25 s | nothing — two rooms this close are in sync as far as anyone can tell |
| 0.25 s to 5 s | the guest's playback rate is nudged by up to ±3%, pitch-corrected, until the gap closes to 0.10 s |
| 5 s or more | a hard seek to the host's position |

The two thresholds in the middle row are not a typo: a correction is *entered* at 0.25 s and *left*
at 0.10 s. One threshold instead of two is what makes a rate controller oscillate — a drift sitting
exactly on the boundary would arm and disarm on alternate beacons for ever.

±3% is the ceiling because that is the point at which a pitch-corrected rate change stops being
imperceptible. It also sets the top of the nudge band: at 3% a nudge closes 0.03 s per second, so
five seconds of drift would take nearly three minutes to walk off — which is why five seconds is
where the hard seek takes over instead.

A beacon that arrives claiming to be from the future, or that is older than thirty seconds, is
ignored rather than acted on. Extrapolating from one bad clock reading would hard-seek the whole
room.

## When someone's stream stalls

A guest whose stream is running dry tells the room, and everyone can see it in **Who's watching**.

"Running dry" is about the guest's own **buffer**, not about whether its picture is moving. Each box asks
its player how many seconds of the film it already holds ahead of the playhead (mpv's
`demuxer-cache-state`: its `cache-duration`, and its `eof` flag for "the rest of the file is already
here"):

| Seconds held ahead | Was it buffering? | Now |
| --- | --- | --- |
| under 1 | no | buffering |
| 1 up to 3 | no | fine |
| under 3 | yes | still buffering |
| 3 or more | either | fine |
| the rest of the film is already here | either | fine |
| the player can't say | either | unchanged |

Whether the player is paused plays **no part**. That matters because "pause for everyone" pauses the
guest who stalled, too. An earlier version decided a paused player had recovered: the room resumed the
moment it paused, the guest was pulled forward past what it had, and it stalled again. The badge blinked
every few seconds and the room never really waited. Two thresholds, one to start buffering and one to
stop, are what keep a buffer that hovers around one number from flipping the whole room back and forth.

While the room waits for a guest, that guest is not moved forward to the host's position, because that
would land it beyond its own buffer. It picks up exactly where it stopped, and anything left over once
the film resumes is ordinary drift (see above).

What the room *does* about a stall is your decision, not ours:

**Settings ▸ General ▸ Watch together ▸ "When someone's stream stalls"**

- **Pause for everyone until they catch up** (the default) — the room waits until **everyone** who
  stalled has three seconds in hand again, then resumes by itself, once.
- **Keep going and show who's behind** — nothing stops; the stall is just visible.

A watch party has a social answer to this, not a technical one, which is why it is asked rather than
guessed. The host's setting is the room's setting, and changing it reaches a room that is already
running: switching to "keep going" while everyone is waiting releases the wait then and there.

An automatic wait is never confused with a pause you pressed. If you pause the film by hand while
someone is buffering, their recovery does **not** start it again.

## Someone who cannot play it

They stay in the room. They are told why on their own screen, everyone else sees "couldn't play
this" beside their name in **Who's watching**, and nobody is dropped and nobody is blocked. They
simply cannot steer the transport while they are not watching, and their stall reports stop counting
towards a wait. If a second source answers for them later, they rejoin the film in progress and are
a full participant again.

## Leaving and coming back

**Leave the room** says so on the way out. If the room was waiting on you when you left, it stops
waiting. Coming back with the same code is a *rejoin*, not a second person: one row in the list, and
you land at the host's position again.

## What is deliberately absent

- **Voice and text chat.** People have Discord. This is a transport channel, not a chat client.
- **More than a small room.** Same honesty netplay takes about being a two-player feature: the
  relay pairs two peers, so a room is two boxes. The room model itself counts participants, so this
  is a transport limit and not a design one, but nothing here pretends to scale to a cinema.
- **Syncing with Jellyfin or Stremio peers.** SyncPlay's own protocol, Stremio's, and this one are
  three different things; bridging them would mean agreeing on how *they* name what to play, which
  is exactly the part this design keeps local.
- **Any hardening beyond what the relay already does.** Same posture as netplay: small rooms,
  people you gave a code to. A room joined on your own network still has to prove it holds the code
  before the host commits to it, but nothing here is an authentication system.
- **Broadcasting anything when there is no room.** The host's opens follow the room only while a room is
  open, a guest's opens never leave that machine, and nothing about what you watch alone is ever sent.
  **Play this for everyone** exists for the other order of events — hosting a room around something that
  was already playing, which has no load event left to fire.
