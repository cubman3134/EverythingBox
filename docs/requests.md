# Requests — asking for something you do not have

Browsing turns up things you cannot watch: a film your library never had, a series you are three
seasons behind on, a title an add-on catalogue lists and no source can stream. If your household
already runs a request service in front of an *arr stack, EverythingBox can ask it for them from the
detail page you are already looking at.

Set up: **Settings → Requests → "Request service…"**.

## What it needs

The address of your [Jellyseerr](https://github.com/fallenbagel/jellyseerr) (or Overseerr) instance
and an **API key** from it — *Settings → General → API Key* in that service's own web UI.

The address is checked before the key is stored, and the key is checked before either is: a
credential that cannot work is never written down. If the address is plain `http://`, you are asked
first, in as many words — the key rides a header on **every** call, so plain HTTP puts it on the
network in clear each time.

**The key never leaves this device.** It lives in the device-local part of the settings, deliberately
excluded from the synced settings bundle, so it is never uploaded to your cloud storage and never
copied to another machine. Set the service up again on the second device. Nothing in the app ever
prints, logs or displays it, and no message you can see is built out of a request — the sentences
this feature shows you are a fixed list of its own, because the alternative (Qt's own error text)
embeds the URL that carried the key.

## Where the button is

On the detail page of any **film or series**, next to Play, on both layouts. Not only on rows that
came from a media server: a catalogue title you cannot stream is exactly the thing worth requesting.

It appears when the item carries a **TMDB or IMDB id** — which is what identifies a title to the
service — and on nothing else. An item with neither shows no button at all rather than one that
would fail when pressed. Games, music, books and local files never grow one.

An IMDB-only item still works: the service is asked to resolve the id first, then asked about the
title it names.

## What the button says

The label is the state, and that is the point:

| It says | It means | Pressing it |
| --- | --- | --- |
| **Request** | nobody has asked for this | asks (after confirming) |
| **Request what is missing** | some seasons are already in your library | asks for the rest |
| **In your library** | the service says your server already has it | **opens it** |
| **Waiting for approval** / **Approved** / **Being fetched** | already asked for | nothing — it is a badge |
| **Request (status unknown)** | the service could not be reached | still asks, and says so first |

**"In your library" can never create a duplicate.** That is the anti-duplicate check the service's
own web UI performs, and it is done again immediately before every ask — the status you were shown
when the page opened may be minutes old, and somebody else in the house may have requested the same
thing in between. Where exactly one Jellyfin server is connected, "In your library" deep-links
straight into playback. With several connected it does not guess which one holds the copy: the badge
stays, the link does not.

## Asking

Every request is an explicit press with a confirmation card in front of it that names the title.
Nothing is ever requested automatically, nothing is requested as a side effect of *viewing* an item,
and nothing is ever retried silently — a request that fails tells you and stops, because a quiet
retry is how one press becomes two downloads on somebody else's disk.

For a series you are asked **whole series or one season**, and only when that is a real question —
the picker offers the seasons that are actually missing. Opening an episode's page and pressing
Request asks for **that episode's season**: a season is the smallest thing an *arr stack fetches.

The **quality profile is your server's default**. It is not exposed here, and nothing this app sends
overrides your own configuration.

## The Requested shelf

Everything this profile has asked for appears on the home screen under **Requested**, grouped by what
you would do about it:

- **Ready to watch** — it arrived;
- **On the way** — asked for, approved, or being fetched;
- **Needs attention** — declined, or the pipeline could not get it;
- **Status unknown** — we could not ask. Never shown as "pending": that would be a claim about
  somebody else's server that we have no basis for.

The shelf does not exist until you make a request, and the row can be moved or hidden like any other
under **Settings → Home screen**.

Statuses are **fetched when you look**, never polled: once per title when its page opens, and once
per row when the home screen is built. There is no background timer, so an idle EverythingBox makes
no requests of your service at all.

The rows live on this device with the credential, for the same reason: a request is a fact about the
service *this* machine is linked to, and a second machine with a different service (or none) could
never refresh them.

## What it deliberately does not do

- **No admin.** Approving other people's requests, declining them, managing users — that is your
  request service's own web UI, and it is a better place for it.
- **No quality-profile picker.** Server defaults, v1.
- **No talking past the service.** No Radarr or Sonarr configuration here. The request service *is*
  the abstraction over them; configuring them separately would double the setup for nothing.
- **No second backend yet.** The Request button and the Requested shelf are built over a backend
  seam with one implementation today. EverythingBoxServer's own request queue slots in behind the
  same surface later — the same button and the same shelf, not a parallel set.
