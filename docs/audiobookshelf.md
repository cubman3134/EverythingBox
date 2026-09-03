# Audiobookshelf

EverythingBox can read an [Audiobookshelf](https://www.audiobookshelf.org/) server you already run. Your
books browse by series and author, your podcasts browse by show, and — the part that matters if you listen
on more than one thing — **the listening position is the server's**, not ours. Where you are in a book is
the same here as in every other app you use with that server.

## Setting one up

Two doorways, both of which do the same thing:

* **Settings → Audiobooks → "Add an audiobook server…"**
* the **Audiobooks** category → **Audiobook Servers** → **"＋ Add an audiobook server…"**

You are asked for a name, the server's address, and your username and password. If the address is plain
`http://` you are asked, once, to confirm that you want your sign-in sent unencrypted — use `https://` if
your server supports it.

**Your password is not stored.** It is sent once, to the server's `/login`, which answers with an API
token; the token is what is kept. That token never leaves this device: it is held in the device-local part
of the settings, which is deliberately excluded from the synced settings bundle, so it is never uploaded to
your cloud storage and never copied to another machine. If you sign in on a second device, you sign in
there.

You can add **as many servers as you like**. Every id this feature stores names the server it came from, so
two servers can hold books with the same internal id without either of them opening the wrong one.

To remove one, long-press (or right-click) its row under **Audiobook Servers**. That forgets the sign-in
here; nothing on the server itself is touched.

## Browsing

Under **Audiobooks → Audiobook Servers → *your server*** you get its libraries. Then:

* an **audiobook library** offers **Series**, **Authors** and **All Books** — Series and Authors appear only
  if your library actually uses them, so a flat collection stays a flat list;
* a **podcast library** is its shows, and a show is its episodes, newest first;
* a **book** shows a **Play book** row and, under it, the book's parts.

Covers come from the server and are cached locally like any other artwork.

## Playing

Pressing **Play book** opens the book on the server and plays it as **one queue**, whether it is a single
`.m4b` or fifty-seven MP3s — the same machinery a local multi-file book uses, so it plays straight through
across a part boundary and the sleep timer, per-book speed memory and background listening all behave
exactly as they do for a book on your own disk.

**Chapters are the server's.** Audiobookshelf publishes an explicit chapter list for a book, and that list
is what the player navigates — including on a book split into many files, where the chapters are the book's
and the file that is open is only part of it. "Sleep at the end of this chapter" therefore means the real
end of the real chapter.

## Your position

Your listening position belongs to the server:

* it is **reported to the server** as you listen (and immediately when you seek, or when you leave the
  book);
* it is **read back from the server** when you open a book — including on a device that has never played
  it before;
* the server's answer **wins** over anything this device remembers about that book.

Because of that, a server book's position is deliberately **not** copied into EverythingBox's own synced
"continue watching" data. One thing owns it, and that thing is your server — which is the whole reason to
run one.

## What is not here yet

This is the first increment of the feature. Not yet supported:

* **downloading for offline listening** — a server book needs the server;
* **following a podcast** so new episodes appear on their own;
* **the book-scale position bar** for a *multi-file* server book: the bar and the time readout show your
  place in the part that is playing, not in the whole book. (A single-file book is unaffected, and the
  chapter list is the whole book's either way.);
* **ebooks** held in an Audiobookshelf library — EverythingBox reads books through its own reading library
  and OPDS instead;
* **Audiobookshelf's admin surface** — users, library scans, and anything else that changes the server.
