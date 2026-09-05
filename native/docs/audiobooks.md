# Audiobooks

Point **Settings → Audiobooks folder** at a folder and everything under it is an audiobook.
Nothing about a *file* decides that — the same `.mp3` under your music folder is music. The
folder you chose is the statement, and you can change it in one press.

A **book** is the audio files of one folder that share one book identity: a folder of numbered
MP3s is one book with one tile, one queue and one progress bar; a single `.m4b` is one book;
a series directory holding one directory per book is one book per directory. Books are browsed
by **Author**, and beside that by **Narrator** and by **Series** — two views over the same
books, offered only when something in your library actually names one.

## Metadata matching

Audiobook tagging in the wild is often a folder name and a bitrate. When a book's own tags
leave a blank, EverythingBox can fill it in from an online provider.

**What can be filled in**

| Field | Where it shows |
| --- | --- |
| Narrator | the book's credit line, and a real **Narrators** shelf entry |
| Series and position | the **Series** view, and the order books sit in inside it |
| Cover | the tile and the book's page |
| Description | the book's page |
| Publication year | the book's page |
| Runtime | shown as a fact on the match; never used as a part's length (see below) |

**Your own tags always win.** This fills blanks and nothing else. A field your files carry is
never overwritten — not by any provider, not at any confidence. A tag that is *present but
empty* (`NARRATOR=""`, or nothing but spaces) counts as a blank, because that is what a bulk
converter leaves behind, not something you said.

**Where it comes from.** Two providers ship with the app and are on by default:

- **Open Library** — official, public, keyless. The only one of the two that states a
  **narrator**, out of an audiobook edition's contributor credits.
- **Google Books** — official, public, keyless. Better descriptions and covers; states no
  narrator at all.

Both are ordinary addons (`native/addons/openlibrary`, `native/addons/googlebooks`), reached
the same way every other provider is. **Any addon that declares `metaFor: ["audiobook"]` is an
equal provider** and its answers are merged in field by field, the same way the four game
artwork providers already work.

There is deliberately **no Audible support compiled into the app**. Audible's catalogue is
what the audiobook world leans on for narrator/series/ASIN and it has no official public API —
the tools that read it use unofficial endpoints. A source like that belongs behind an addon you
choose to install, or nowhere.

**A series position comes from the match, never from a title.** "Book 3 of …" in a title is as
often a boxed-set volume or a publisher's numbering as it is the place a shelf sorts by, so it
is not read. Only a provider's own position field is.

**A part's length is never taken from a match.** The progress bar divides by the lengths read
out of your files. A provider's runtime is for *their* edition of the whole book, so it is shown
as a fact and never becomes a number the player relies on.

## Seeing and rejecting a match

A book that was matched online gains one extra row on its own page:

> **ⓘ Matched online** — *what was matched · the author · the narrator · a confidence %*

Open that row and you land in the ordinary per-item metadata editor ("Fix info"), which gains a
row of its own:

> **✗ Reject match: …**

Rejecting is permanent. The book goes straight back to exactly what its own tags say, the
cached cover and description are dropped, and it is **not matched again** — not on the next
scan, not on the one after that. The rejection is stored beside your other corrections in the
portable `everythingbox.ini`, so it syncs to your other devices and survives a re-scan, a
metadata cache wipe and a reinstall of the app over the same folder.

Nothing is written back into your files. Matching never re-tags anything on disk, and the
persisted library index holds only what the tags said — which is what makes a rejection able to
put everything back.

## When nothing happens

- **No provider addon enabled** → the feature is dormant and costs nothing.
- **A book whose tags already carry a narrator, a series, a year and a cover** is never asked
  about at all.
- **A weak match is no match**: below the confidence threshold, nothing is stored and nothing
  is shown. A book with no match looks exactly as the scan found it — no placeholder, no
  half-filled record.
