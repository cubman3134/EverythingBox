# Romhack patches: let the response decide how it should be fetched

**Date:** 2026-08-26
**Follows:** 262996b (`Merge: serve a romhack's file instead of embedding it in the response`)

## The problem

The romhack install flow fetches the patch file inline, with `fetchUrlBlocking`, in
`MainWindow::showRomhacks`. That is right for the IPS/BPS/UPS case — a few kilobytes, arriving
inside a flow that is synchronous by design because every step needs the previous answer. It is
wrong for the case the code's own comment names beside it: an xdelta patch built against a disc
image is itself disc-scale.

Three faults follow from the same decision.

**No `.part`, no resume, no progress, no cancel.** `fetchUrlBlocking` buffers the whole body into a
`QByteArray` and hands it back. A dropped connection at 400 MB is 400 MB thrown away, with nothing
on screen having said how far it got and no way to have stopped it. At disc size a dropped
connection is not a rare event.

**The deadline is a hard 180 s, and at disc scale it is a lie.** A 500 MB patch over a 10 Mbit link
is about 400 s. It hits the ceiling every time, and reports:

> Couldn't download X's patch — try again.

Trying again produces the same sentence. The message names a source problem for what is a client
timeout, so the one place someone would look is the one place with nothing wrong in it.

**A patch that does land is held whole, for as long as the install takes.** `PendingRomhack`
carries `QByteArray patchBytes`, and `RomhackInstall::install` runs with the base ROM and the
patched output live at the same time. That is the whole-file-in-memory shape the streaming arc
existed to remove — relocated from the server to the client rather than removed.

It is worse than the field suggests. Both async routes capture the request by value:

```cpp
const PendingRomhack pending = req;      // by value: this outlives the frame that built it
```

which copies the entire patch into the lambda. A 500 MB patch is held **twice**, for however long
the base ROM download takes.

### What already works, ten lines up

The finished-game route in the same function solves all of this. It builds a `DownloadJob`, hands
it to `DownloadManager`, and gets streaming to a sibling `.part`, a Range request on resume, and
progress and Cancel in the Downloads panel. Its `jobCompleted` one-shot, its `deferPastQmlEmission`
before anything that opens a nested loop (#28), and its in-flight guard set are the patterns this
design copies rather than reinvents.

### Why patches were not queued, and why that reasoning still holds

Deliberate, and still correct as far as it goes:

* `DownloadManager` runs **one job at a time**. A 14-byte IPS queued behind a 40 GB disc image is a
  hang from the outside, whatever the panel says.
* Every completed job is recorded in Recent and the Downloaded folder. A raw `.ips` sitting there
  is noise — nobody asked for that file, they asked for a game.

So the small case must stay off the queue and out of the record. Only the large case wants resume
and cancel. Which means the flow needs a **size signal** — and it cannot guess one from the format:
`PatchFormat` is asserted by the source and never sniffed, and a `.bps` total conversion and a
`.bps` two-byte tweak are the same string.

## The design

### 1. The size signal is the fetch's own response head

The fetch is already a GET, and a GET's response head already declares `Content-Length`. So the
size signal costs nothing extra: the request judges itself, at its own first byte.

New unit `native/src/core/BoundedFetch.{h,cpp}`. It exists as a unit rather than as another
`static` in `MainWindow.cpp` because it is the only part of this change with real logic in it, and
a `static` buried 13 000 lines into a translation unit with a `QQuickWidget` in it has no way to be
tested at all.

```cpp
namespace BoundedFetch
{
    struct Result
    {
        enum Verdict { Ok, TooBig, Failed };
        Verdict    verdict  = Failed;
        QByteArray body;          // the whole body, and only when verdict == Ok
        qint64     declared = -1; // Content-Length, when the response stated one
        int        status   = 0;  // HTTP status, or 0 when the request never got a response
        QString    error;         // Qt's reason, for the log — never for the user
    };

    Result get(const QString& url, int timeoutMs, qint64 ceilingBytes);
}
```

One GET, on `NoLessSafeRedirectPolicy` — the same policy the call site holds today, for the same
stated reason: the request carries no headers, no cookies and no credentials, so a cross-host 302
leaks nothing, and a server behind a reverse proxy needs the hop followed or its files are
unreachable.

The decision is made on the **first `readyRead`**, not on `metaDataChanged`. That is the point that
matters: `metaDataChanged` fires once per redirect hop, so a 3xx head can be read as the real one,
whereas body bytes only ever arrive on the final response. By the first `readyRead` the head being
read is the head that describes the bytes.

There:

* `Content-Length` over the ceiling → abort, `TooBig`, `declared` set.
* No `Content-Length` → keep accumulating, and abort the moment the running total crosses the
  ceiling → `TooBig`, `declared` stays `-1`.

A chunked or connection-delimited response therefore takes the same path as a declared one, with no
branch of its own. `declared` is reported rather than inferred, because "the server said 500 MB"
and "the server said nothing and we stopped at 16 MiB" are different facts, and only the first can
be put in a sentence.

An abort arrives as `OperationCanceledError`. That is the one error this must **not** report as a
failure — a flag set before `abort()` decides which, the distinction `DownloadManager` already has
to draw with `redirectRefused_` for the same reason.

**The ceiling is 16 MiB** (`16 * 1024 * 1024`, a named constant at the call site — `BoundedFetch`
itself takes the number as a parameter and holds no policy). Above essentially every cart-era
patch, so NES/SNES/GB/GBA/Genesis/N64 IPS, BPS and UPS stay instant, off the queue and out of the
record. Above it is disc-era xdelta, which is exactly what wants resume and cancel.

**The inline deadline drops 180 s → 60 s**, matching the flow's other waits. With the size bounded
at 16 MiB, a 60 s timeout means something is genuinely wrong — which is the first time the sentence
"couldn't download it, try again" will have been true.

**A `Failed` says which failure it was.** The brief's complaint is not only that the fetch times
out, it is that it times out *with nothing to diagnose*. So `status` and `error` are carried out,
the user-facing note distinguishes the two cases someone can act on —

* a response that refused (`status >= 400`): the server answered, and the file is likely gone from
  its timed store, so the sentence says to fetch the hack again rather than to retry;
* no response inside the deadline: the sentence says the source did not answer in time;

— and the detail goes to the debug log through `mwLog` with the URL passed through `logSafeUrl`,
both of which already exist at the top of `MainWindow.cpp`. That shape is not incidental: the
log-discipline gate matches log calls by the `…Log(` name, so a helper under some other name would
be a hole in it, and `logSafeUrl` is what keeps a host and a query string out of the file
(e66fa9a). `BoundedFetch` itself logs nothing — it reports, and the call site decides.

The user-facing strings never carry `error`: Qt's reason text is not a sentence anyone can act on,
and one of its values is the literal "Operation canceled".

### 2. `PendingRomhack` carries a path, not bytes

```cpp
QByteArray patchBytes;   // →
QString    patchPath;    // where the patch was cached
```

Both routes write the patch to a cache file, and `applyRomhack` reads it immediately before
`RomhackInstall::install`. The by-value lambda captures become free, and the hour-long hold across
a base-ROM download goes away.

**The limit, stated rather than implied:** `RomPatch::apply` is in-memory by design and already
refuses anything past `INT_MAX`, so the patch bytes are still resident *during the apply*. What
this removes is the duration and the duplication, not the peak. A streaming applier is a different
and much larger piece of work, and is out of scope here (see below).

`RomhackInstall::install` keeps its `QByteArray` parameter and is not touched. The caller reads the
file; the applier needs the bytes in RAM either way, and an overload that only moved the `readAll`
one function deeper would buy nothing.

**Where the file lands:** `<data>/downloads/patches/<hackId>-<patch file name>`. Stable and
derived, which is what makes `enqueue()`'s de-dup-by-destination give resume across a restart for
free.

**Checked before any network.** An existing non-empty file at that path is used directly, and the
flow proceeds as if the fetch had just succeeded. This also sidesteps a trap the `rom` branch
already documents: letting `enqueue()` notice the file itself makes it emit `jobCompleted`
**synchronously**, into a handler that ends in `NavConfirm` — the #28 shape.

**Lifetime:** deleted after a successful install, because it is an intermediate. Left in place
otherwise — that is precisely the retry cache, and it is what makes a second press after a failure
cost nothing. Patch files older than 7 days are pruned on entry to the flow, so an abandoned
disc-scale patch does not live in the data dir forever.

### 3. `DownloadJob::record` — the unrecorded half

```cpp
// Whether finishing this job means the user asked for the FILE. A romhack patch is an
// intermediate: it belongs in the Downloads panel, where its progress and its Cancel are,
// and nowhere else. false keeps it out of Recent and the Downloaded folder.
bool record = true;
```

Persisted, defaulting to `true` on load so an existing `queue.json` behaves exactly as before.

Honoured by a single early return in the `jobCompleted` handler at `MainWindow.cpp:663` — which
suppresses the `RecentStore::add`, the `DownloadsStore::add` **and** the "Downloaded X" toast
together, because all three are that handler and all three are wrong for a patch.

A `kind` string would not do this. Nothing at that site reads `kind`; `"patch"` would be recorded
identically to `"game"`, and the bug would be that the field looks like it should have worked.

The Downloads panel is unaffected — it renders `dm_->jobs()`, so the patch keeps its row, its
progress bar and its Cancel. That is the entire purpose of routing it there.

**The job the patch route builds**, stated here so it is not guessed at:

| field | value | why |
|---|---|---|
| `title` | `tr("%1 (patch)").arg(hack.title)` | the panel row has to read as an intermediate, not as the game — someone scanning Downloads should not think the hack has already arrived |
| `url` | `RomhackClient::fileUrl(server, patch.url)` | the same URL the inline fetch would have used |
| `dest` | the cache path from §2 | stable, so de-dup gives resume |
| `key` | `hack.id` | the only handle back; the job id is minted inside the manager and never returned |
| `kind` | `"patch"` | nothing routes on it, but a job in `queue.json` with an empty kind is unreadable to anyone looking at the file |
| `record` | `false` | §3 |
| `thumb` | the base game's `thumbnailUrl` | the row is otherwise blank, and the artwork is what identifies which install this belongs to |
| `sysId` | empty | a patch is not a game on a system; the installed result will be |

`kind` is `"patch"` and *not* `"game"` on purpose. It changes no behaviour today — `record` is what
suppresses the recording — but a later reader looking for how patches are kept out of the library
should find a kind that says what the file is, rather than one that lies and a boolean that
corrects it.

### 4. Every question above every transfer

The base-ROM-missing confirm moves **above** the patch acquisition. It is a question about the ROM
and needs no patch to answer — the same reasoning already written down beside the target warning,
which is asked early so nobody is interrupted at the end of a long download to be told the thing
they chose might not fit.

Without the move, a queued patch produces exactly that: "Download the game too?" arriving several
minutes after a 500 MB transfer finished, to someone with no memory of having started it.

New order in `showRomhacks`:

1. list → pick hack → fetch → pick patch file
2. the `rom` (finished game) branch — untouched
3. target / translation warning confirm
4. **base ROM present? if not, the "Download both?" confirm** ← moved up, recorded as a flag
5. acquire the patch:
   * cached file present → use it
   * else `BoundedFetch::get(url, 60'000, kPatchInlineCeiling)`
     * `Ok` → write it to the cache path, continue on this frame
     * `TooBig` → enqueue a non-recording `DownloadJob`, arm the one-shot, **return**
     * `Failed` → report, and end the flow
6. base ROM needed → `downloadBaseRomThenApply`, else `applyRomhack`

After step 4 there are no more questions, only transfers — ending in the usual deferred
"Play it now?".

### 5. Async safety, copied from the route that already paid for it

Queuing the patch makes the rest of the install asynchronous, so `showRomhacks` returns and
`romhackBusy_` is released while a transfer is still out. That is the same situation the finished-
game route is already in, and it gets the same three defences.

**Its own slot and one-shot,** `pendingRomhackPatch_` and `romhackPatchConn_`, mirroring the
base-ROM pair rather than sharing it. Both can be live *in sequence* for a single install — patch
first, then base ROM — and a shared slot makes the disarm ambiguous at exactly the moment it
matters.

**`deferPastQmlEmission` before the continuation.** `jobCompleted` is emitted from inside
`finishActive`, which still holds a live reference into its own `jobs_` vector and has not yet
cleared `activeId_`, saved, or pumped. The continuation reaches `NavConfirm`, which is a
`QEventLoop`. Running one there is the documented crash class (#28). Nothing index-like survives
the deferral: the pending request is already a copy, and the job's destination is resolved to a
plain string before the hop.

**An in-flight set keyed on hack id,** `romhackPatchDownloads_`, honouring only the **first**
completion per id — a cancel-then-retry leaves an older handler armed with no way to disconnect it,
and two that both ran would stack a second confirm inside the first's nested loop. The refusal is
gated on the manager *still holding* the job, because `cancel()` drops a job without ever emitting
`jobCompleted`, and an id left in the set until the process ended would tell a perfectly reasonable
retry a lie. A `Failed` or `Paused` job says it is stopped and names Downloads, rather than sending
someone to watch a bar that will never move.

**The progress note is bounded, not sticky.** `cancel()` emits nothing, so a sticky note goes on
announcing a transfer that stopped. The line only has to say the transfer started; the panel holds
the progress and the Cancel, and that is where anyone watching should be looking.

### 6. The probe

`probe_romhack` gains a section driven by a local `QTcpServer`, the pattern `probe_serversync` and
`probe_stremio` already use. It pins the decision, not the plumbing:

| case | asserts |
|---|---|
| declared length under the ceiling | `Ok`, body is byte-exact, `declared` matches |
| declared length over the ceiling | `TooBig`, and **only a few KB were read** — the abort happened at the head, not after the body |
| no `Content-Length`, under the ceiling | `Ok`, body byte-exact, `declared == -1` |
| no `Content-Length`, over the ceiling | `TooBig`, aborted mid-stream, bytes read bounded near the ceiling |
| 404 | `Failed` |
| head sent, body stalls | `Failed` on the deadline, and the call returns near the deadline rather than hanging |

The over-ceiling cases assert the **byte count**, not just the verdict. A `TooBig` returned after
quietly reading all 500 MB is the exact bug this design exists to prevent, and it would otherwise
pass a verdict-only assertion.

Existing sections are unchanged and no new probe is registered — this extends one that already
exists in all three places.

## Out of scope

**A streaming patch applier.** `RomPatch::apply` is in-memory, and making it stream is a change to
four format decoders with its own correctness burden. This design shortens how long the bytes are
held and stops them being held twice; it does not remove the peak.

**A server-declared `patches[].size`.** Exact, free at the point of choice, and it would let the
chooser row read "xdelta, 480 MB". But it is a cross-repo API bump — public server plus private
plugin — that cannot be built or verified from this repo, and an older server would send nothing,
so the response-head path would still have to exist underneath it. If the field is ever added,
`BoundedFetch` degrades into the fallback it already is; nothing here has to be undone.

**A HEAD probe.** Pays a round trip on every install for the common case that needs none, and
depends on the file route answering HEAD at all — which this repo can neither see nor test.

**Concurrency in `DownloadManager`.** One job at a time is unchanged. The small case avoids the
queue entirely, which is what the one-at-a-time constraint actually required.

## Success criteria

* A kilobyte IPS installs exactly as it does today: no Downloads row, no Recent entry, no
  Downloaded-folder entry, no perceptible delay.
* A patch over 16 MiB appears in the Downloads panel with a progress bar and a working Cancel,
  survives a dropped connection via a Range resume, and continues the install by itself when it
  lands.
* Cancelling a patch download leaves no stale handler, no stranded sticky note, and no id that
  refuses a later retry.
* Re-pressing a hack whose patch is already cached does no network work at all.
* No patch is copied into a lambda; `PendingRomhack` is cheap to copy.
* The "couldn't download, try again" message is reachable only when something is actually wrong.
* `probe_romhack` fails if the over-ceiling case reads the whole body.
