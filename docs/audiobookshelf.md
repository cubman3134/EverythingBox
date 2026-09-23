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
* a **book** shows a **Play book** row, a **Download for offline** row (or **Remove download** once it is on
  this device), and, under them, the book's parts.

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

**The position bar is the whole book's.** On a book split into many files, the bar and the time readout
show your place in the *book* and its full length — not the part that happens to be playing — because the
server says how long every part is. Dragging or clicking the bar goes to that point in the book, moving to
another part when that is where the point falls.

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

## Listening offline

**Download for offline** on a book's page keeps a copy of the book on this device. Every audio file of the
book is fetched — through the ordinary Downloads queue, one entry per file, so **Settings → Downloads** shows
their progress and can pause, retry or cancel them — and when the last one lands the book appears **once**,
as one book, under **Audiobooks → Downloaded books**, which lists every server book on this device and
opens it with the server switched off. It keeps its title, author, narrator, cover and the
server's chapter list, and it knows how long every part is, so the whole-book position bar works without
the server too. The files are fetched with the server's own download route
(`/api/items/<id>/file/<file>/download`), so an account the server does not allow to download is told so
rather than quietly given the files anyway.

**Your sign-in is not written into any of it.** A download in the queue is named by the book and the file,
never by a link; the link — which has to carry the server's token — is made at the moment each file is
requested and is not kept. A download interrupted by a restart picks up again the same way.

**Opening a downloaded book plays it from this device**, from **Downloaded books**, from Recents, or from
the book's page on the server — with the server switched off, it still plays.

**Your position is still the server's.** While the server can't be reached, the position this device reports
for a downloaded book is **kept**, on this device only (it is not part of the synced settings), and only
the latest place in each book is kept, not every moment. The next time the server answers — the next time
you browse it, or listen to a downloaded book while it is up, or start the app, and in any case within
half a minute of it coming back while the app is open — the kept positions are **sent**. When you open a downloaded book and the server answers, **the server's position is used, unless
the position this device kept is newer than the server's last update** — for instance, you listened on the
plane and have not opened the book anywhere since. Then that position is sent to the server first, and the
book opens there. So listening offline moves your place forward everywhere, and never drags another
device's newer place backwards.

**Remove download** on the book's page deletes this device's copy — its files, its Downloaded entry and any
position kept for it. The book on the server is not touched.

## What is not here yet

Not yet supported:

* **following a podcast** so new episodes appear on their own (podcast episodes are not downloaded either);
* **ebooks** held in an Audiobookshelf library — EverythingBox reads books through its own reading library
  and OPDS instead;
* **Audiobookshelf's admin surface** — users, library scans, and anything else that changes the server.
