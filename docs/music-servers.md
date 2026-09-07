# Music servers (Subsonic)

EverythingBox can play a music server you already run — [Navidrome](https://www.navidrome.org/),
Airsonic, Gonic, Ampache, Astiga, or anything else that speaks the Subsonic API. Your artists, albums
and tracks browse exactly like music on this device, because they are drawn by the same screens: a
server is another **supplier** of the one music library, not a separate app bolted on the side.

## Setting one up

Two doorways, both of which do the same thing:

* **Settings → Music → "Add a music server…"**
* the **Music** category → **Music Servers** → **"＋ Add a music server…"**

You are asked for a name, the server's address, and your username and password. If the address is plain
`http://` you are asked, once, to confirm that you want your sign-in sent unencrypted — use `https://`
if your server supports it.

**Your password never leaves this device.** It is held in the device-local part of the settings, which
is deliberately excluded from the synced settings bundle, so it is never uploaded to your cloud storage
and never copied to another machine. Every request is signed with a fresh salted token derived from it;
the password itself is not on the wire, and no error message anywhere in the app is built out of a
request URL, because for this protocol the URL *is* half the credential.

You can add **as many servers as you like**. Every id this feature stores names the server it came
from, so two servers can hold a playlist or an album with the same internal id without either of them
opening the wrong one.

## What you can browse

Opening a server gives you its artists, and under them its albums and tracks — plus three levels that
are the server's own answers rather than a walk of the tree:

| Level | What it shows |
|---|---|
| **Playlists** | The playlists saved on the server. Open one and it plays like an album, **in the order somebody arranged it** — the tracks are never re-sorted by the numbers they carry on their own records. |
| **Starred** | Everything you have starred on that server: artists, albums and loose tracks. Pressing a starred track plays the starred list from there. |
| **Recently added** | The newest records on the server. |

Playlists are **read-only** here for now: you can play one and queue from it, but creating and editing
them happens on the server. Downloading a record for offline listening is not part of this either.

## Starring, and your favourites

The **favourite** button is the star. There is no second verb: pressing ★ on a track from a music
server records the favourite here *and* stars it on the server that served it, from every surface that
can star something.

Two rules worth knowing:

* **A failed star does not un-press itself.** If the server is asleep, the favourite still stands here
  and you are told, once, what the server said. Silently reverting the star you just pressed is the
  worse of the two wrong answers — it looks like the button is broken.
* **Reading the Starred level only ever ADDS.** Tracks the server already holds a star for become
  favourites here the first time you open that level. Nothing is ever removed: your favourites include
  films, games and music from other places, and "make the local list match the remote one" would empty
  your shelf.

## Reporting plays back, and the one setting that matters

When you listen to a track from a music server, the app tells that server — so its own play counts and
its "recently played" stay right, exactly as they would if you had used any other client.

That leaves one question, and it is the one that goes wrong quietly, so it is a **single setting**
rather than two that can contradict each other:

**Settings → Music scrobbling → "My music server scrobbles for me"**

| | What happens |
|---|---|
| **Off** (default) | A track played from a music server is reported **to the server and to the listening services you have connected here** (Last.fm, ListenBrainz). This is right when your server forwards nothing of its own, which is the ordinary setup. |
| **On** | It is reported **to the server only**, and the server passes it on. Turn this on if Navidrome (or Airsonic, or Gonic) is signed in to Last.fm or ListenBrainz itself — otherwise every one of those plays is counted twice, and the only symptom is a history that says you listened to everything exactly twice. |

Note what the setting does **not** touch: music on this device and streams from add-ons are scrobbled
the same way whichever position it is in, because there is no server in the middle of those to forward
anything. And the server is told about the play in *both* positions — it can only forward what it
hears.

Everything else about scrobbling is unchanged and shared: a track counts once you have played half of
it or four minutes, whichever comes first; listens that could not be delivered are kept, with their
original time, and go out when the network comes back. Each server gets its own delivery counter and
its own "waiting to send" line in Settings, so one box being switched off never hides another working.

## Removing a server

Long-press (or right-click) its row under **Music Servers**. That forgets the sign-in here and stops
reporting plays to it; nothing on the server itself is touched.
