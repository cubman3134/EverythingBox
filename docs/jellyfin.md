# Jellyfin

EverythingBox can browse and play a [Jellyfin](https://jellyfin.org/) server you already run. Add as many as
you like under **Settings → Jellyfin → "Jellyfin servers…"**; their libraries merge into your shelves, each
row tagged with the server it came from, and a server that is switched off or unreachable simply contributes
nothing rather than emptying the shelf.

Your **sign-in never leaves this device**. Signing in gets an access token, and that token is held in the
device-local part of the settings — deliberately excluded from the synced settings bundle, so it is never
uploaded to your cloud storage and never copied to another machine. Sign in again on the second device.

## Downloads — taking it on a plane

Any film or episode from a Jellyfin server can be kept on this device and watched with the network off.

**Where the verb is.** On a film or an episode: **Download**, next to Play. On a **series** or a **season**:
the same button, which then asks what you want —

* **Download all *N* missing episodes** — the whole season (or the whole series, from a series row), minus
  anything already on this device;
* **Download the next 1 / 3 / 5 / 10 unwatched** — the next few, in season-and-episode order, skipping what
  the server says you have already watched and what is already here.

Downloads join the ordinary queue: one at a time, resumable, pausable, and visible under
**Settings → Downloads** like every other download. They survive a restart and pick up where they stopped.

### What they cost

This increment downloads the **original file**, byte for byte — the same file your server holds. That is the
honest version of "take it with you": it resumes properly over a bad connection, it is exactly what you would
have streamed, and nothing about it is re-encoded. It is also **as large as the file on your server**, which
for a modern film is frequently 20–60 GB. There is no "optimised for device" transcode yet; that is a
separate piece of work, because a transcode is produced on demand and is not resumable the way a static file
is, and pretending otherwise would give you a download that silently restarted from zero every time the
connection dropped.

### Watching it offline

A downloaded item **plays from the local file, always** — even when the server is reachable, and without
asking it anything first. Its artwork and description are the ones cached when you downloaded it, so the
Downloads shelf looks the same on an aeroplane as it does at home.

Your position is kept **on this device** while you are offline. Pause halfway through on the plane, close the
app, open it again: it resumes where you were.

### ...and what happens when you come back

Everything you watched offline is **queued and sent to the server when it is next reachable** — so the server,
and every other device on your account, catches up with where you actually got to. The queue survives a
restart and is kept per server, so one server being down does not hold up another's.

**A queued report will never move your server backwards.** Before each one is sent, EverythingBox asks the
server what it already knows about that item. If the server is further ahead — because you also watched it on
the television at home, or finished it there — the older report from the plane is **dropped**, not applied.
Losing an offline position is a small annoyance; overwriting a newer one on every device you own is not, and
that is the trade this makes deliberately.

Nothing about the download itself is stored with a link in it. What is written down is the **item's id**, and
the download link — which carries your access token — is created fresh for each request and then discarded.
That is why a download still resumes correctly after a restart, and why nothing in the app's files has your
token in it except the sign-in itself.

### Keeping the disk under control

Two settings, both under **Settings → Downloads**, and both about *this* machine's disk (so neither is
included in anything the app syncs):

* **Storage limit for downloads** — off by default. Set one and, when your downloads go over it, you are told
  how much is in use and how many items could be freed. You are shown the **least recently watched** ones
  first. **Nothing is ever deleted for you** — the limit suggests, and you decide.
* **Offer to remove downloads once watched** — off by default. Turn it on and finishing a downloaded item
  offers to free the space it was using. It always asks.

### What is not here yet

* the **optimised-for-device** transcoded download (a smaller file for a small screen);
* downloading while the app is closed — downloads run while EverythingBox is open, like every other download.
