# Play on device

Two EverythingBoxes on the same network can hand playback to each other. Start something on the
laptop, flick it to the box under the television, and control it from where you are sitting; on the
way out of the house, pull it back onto the phone at the second you left off.

It is LAN only. Nothing here goes near a cloud service, and no account is involved.

## Turning it on

**Settings ▸ General ▸ Remote control ▸ "Control from a phone on your network"** — on both devices.

That switch is what already gave each instance a small local HTTP surface; this feature is built on
it, so a device with it off is neither findable nor a target. There is nothing else to enable.

Under **Settings ▸ General ▸ Play on device** each box shows the name other boxes will see it by. It
defaults to the machine's own host name; **Rename this device…** changes it. Clearing the name puts
it back to the host name rather than leaving a blank row in somebody's picker.

## Finding each other

An instance with remote control on advertises `_everythingbox._tcp` over mDNS, carrying its name,
its app version and a stable id for the install. Every other instance on the network is listening on
the same socket it already used to find Chromecasts, so peers appear in the **same picker**:

- during playback, the cast button's menu — "EverythingBox on Den TV" sits beside your Chromecast
  and DLNA targets;
- with nothing playing, **Settings ▸ General ▸ Play on another device…**.

A box never lists itself.

If the other device does not appear, give it a few seconds (mDNS is chatty but lossy), and check the
remote-control switch is on **at both ends** — a box with it off can see others but cannot be seen.

## Pairing, once

The first time you hand something to a device, that device puts a **six-digit code on its own
screen** and you type it here. That is the whole check, and it is deliberately a physical one: being
able to read the code means being in the room. Nothing on the network can pair by asking nicely.

- The code is single-use, and three wrong answers burn it — the target has to be asked for a new one.
- On success the target issues this device a token, kept on this device only. It is never included
  in a settings sync, so a box you have never paired with cannot inherit permission from one you have.
- Every later hand-off to that device is one press.
- If the other device is reinstalled, the token it issued stops being valid; the next hand-off simply
  asks for a code again.

## What is transferred

**A reference and a position. Never the media itself.**

The hand-off carries what to play — a catalogue id, an add-on stream reference, an id on a server
you are both signed in to, or a local file — plus where you are in it and which audio/subtitle
tracks are selected. The receiving device then resolves its own stream, with its own sources and its
own credentials. Video never passes through the device you started on, so handing a film to the TV
does not turn your laptop into a relay, and a link that had already expired on one box is re-fetched
fresh on the other.

**What is NOT transferred:**

- the stream itself, or any link to it;
- your add-on or debrid credentials;
- the queue — the item you are playing moves, the rest of the list does not;
- playback speed, subtitle styling, audio delay and other per-device preferences;
- anything at all if the target cannot resolve the reference (see below).

## When it cannot be done

Some things only exist on one machine. A file in a folder the other box does not have is the obvious
case, and the answer is honest rather than clever: **"Not available on Den TV"**, with the reason.
There is no fallback that streams it out of this device.

A target on a restricted profile refuses an incoming hand-off outright, and says so. Restrictions
are expressed through what a profile can browse, and a hand-off arrives underneath that — so rather
than let it through, it is turned away. Press play on that device itself if that is what you meant.

## Controlling it from here

After a hand-off, this device becomes a remote for the other one: what is playing, where it is up
to, play/pause, skip back and forward, next and previous track, volume where the target has one, and
stop. Leaving the remote does **not** stop the other device — only the row that says "Stop on …"
does that.

The remote works over the same local surface, so it keeps working if you go back to browsing here.
Open it again from **Play on another device… ▸ Remote for …**.

## Continue on this device

The same thing backwards. From **Play on another device… ▸ Continue on this device from …**, this
box asks the other one what it is playing, stops it there, and opens the same item here at the same
position.

If the other box is playing something it cannot name — a file dropped straight onto it, say — the
take-over is refused rather than guessed at from the title.

## Not in this version

- No synchronised playback across several devices at once. This is transfer, not party mode.
- No cloud relay: both devices must be on the same network.
- Video hand-off carries position and track selection only.
