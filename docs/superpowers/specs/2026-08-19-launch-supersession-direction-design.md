# Launch supersession must run both directions: an external launch retires the in-app contexts

## The bug

`2026-08-19-cross-frontend-launch-supersession.md` made the in-app launch tails cancel a
pending EXTERNAL emulator launch: `finishLibretroLaunch` and `finishRetroParkLaunch` both
call `cancelPendingEmulatorLaunch()` before they take the screen. The reverse retire was
never built. `GameLauncher::runEmulator` — the commit point of every external launch —
retires neither of `GameLauncher`'s own per-launch contexts:

* `launchCtx_`, which owns the pending core/BIOS download continuation, is retired only in
  `openResolved`'s libretro tail;
* `extractCtx_`, which owns the archive-extraction continuation, is retired only at the top
  of `open()`.

So supersession is "whichever finishes first wins" rather than "newest wins". The user picks
in-app game A (core download starts, `open()` returns), then external game B (the RPCS3
install/update phase begins). Minutes later A's download lands, `finishLibretroLaunch` runs,
cancels B with a toast naming B, and boots A. The user's last expressed intent loses.

## What is actually uncovered

`open()`'s top retires `extractCtx_` unconditionally, on BOTH the archive and the
non-archive branch. That already covers every `runEmulator` reached through `open()`:

| Path to `runEmulator` | State of `extractCtx_` on arrival |
| --- | --- |
| `open()` (non-archive) -> `openResolved` -> `launchExternalGame` | fresh, unused — a prior extraction was dropped at `open()`'s top |
| `open()` (archive) -> worker -> continuation -> `openResolved` -> `launchExternalGame` | **the executing continuation's own context**; nothing stale in it |
| `MainWindow` "Launch" button / `emu.launch:` row -> `runEmulator(em)` | **a prior launch's live extraction — the hole** |

Two things follow, and they set the whole shape of the fix:

1. The trap case (`runEmulator` reached from inside `extractCtx_`'s own executing
   continuation) is exactly the case with nothing stale to retire. Deleting the context
   there frees the owner of the lambda on the stack — the crash-#28 / `deferPastQmlEmission`
   class — for no benefit at all.
2. The genuinely uncovered entry is the pair of bare `runEmulator(em)` calls that never
   touch `open()`: Settings > Emulators' "Launch" button (`MainWindow.cpp`) and the themed
   panel's `emu.launch:` row. Pick in-app game A, back out, open an emulator's own UI — A's
   extraction AND its core download both survive and boot over it.

`launchCtx_` has no trap at all. `openResolved` returns at its external branch *before* it
touches `launchCtx_`, and the only endpoints of a `launchCtx_` continuation are
`finishLibretroLaunch` and `finishRetroParkLaunch`, neither of which reaches `runEmulator`.
`runEmulator` can never be inside `launchCtx_`'s own call chain, so retiring it there is
unconditionally safe.

## Decision: consume-on-entry

The extraction continuation detaches its own context at its top:

```cpp
contexts_.takeExtractContext(ectx);   // first statement of the continuation
```

After that, `extractCtx_` is null for the remainder of the call chain, so `runEmulator` can
retire BOTH contexts unconditionally: inside the continuation the delete is a no-op, and
from a bare `runEmulator` it is a real, immediate drop. No flag, no counter, no deferral, no
conditional at the retire site. The invariant it states is the true one — *a context whose
continuation has already fired has no queued work left to protect.*

Detaching uses `deleteLater()`, not `delete`: the object still owns the lambda currently
executing. That is safe here in a way it is not at the retire site, because by the time
anything else could look at the slot it is already null — there is no window in which a
still-pending continuation survives a supersession.

Two alternatives were weighed and rejected:

* **Deferred retire (`deferPastQmlEmission` idiom).** Re-opens the exact race being closed.
  Between `runEmulator` and the next event-loop turn the extraction worker can finish and
  its already-queued `QMetaCallEvent` can be delivered before the deferred delete runs; and
  `deleteLater` does not drop deliveries already queued ahead of the destruction. It is the
  right tool for a QML-emission re-entrancy crash and the wrong one for a supersession race.
* **Per-launch generation counter.** Reintroduces the `launchGen_` idiom that was reviewed
  out of `EmulatorManager` in favour of context objects, and duplicates state the context
  pointer already carries — the pointer *is* the generation. Two sources of truth for "which
  launch is current" is how the wrong winner got picked in the first place.

## The unit

`GameLauncher` cannot be built in a probe (a window, a `RetroView`, a `RetroParkView`), so
the retirement protocol moves into a header-only, window-free unit that can be.

### `native/src/launch/LaunchContexts.h` (new)

Holds the two per-launch context objects and the one lifecycle rule — *retiring a slot
destroys its object and installs a fresh one* — plus the single documented exception,
`takeExtractContext`.

```cpp
class LaunchContexts
{
public:
    explicit LaunchContexts(QObject* owner);
    QObject* beginOpen();                  // open()'s top: supersede a prior extraction
    QObject* beginCoreFetch();             // openResolved's libretro tail: supersede a prior core/BIOS chain
    QObject* coreFetchContext() const;     // re-read inside the core-ready continuation for the BIOS chain
    void takeExtractContext(QObject* ctx); // continuation entry: detach (deleteLater) if still current
    void retireForExternalLaunch();        // runEmulator's commit: drop every in-app continuation in flight
};
```

`takeExtractContext` is a no-op when `ctx` is not the current context: that means a newer
`open()` already retired it, and the object is not ours to touch.

### `GameLauncher` changes

* `launchCtx_` + `extractCtx_` members become one `LaunchContexts contexts_`.
* `open()`'s top: `QObject* ectx = contexts_.beginOpen();`
* extraction continuation, first statement: `contexts_.takeExtractContext(ectx);`
* `openResolved`'s libretro tail: `ensureCoreThen(plan, contexts_.beginCoreFetch(), ...)`, and
  the BIOS chain inside it uses `contexts_.coreFetchContext()`.
* `runEmulator`, immediately after the `emu_->busy()` refusal — the point at which the
  external launch is committed — `contexts_.retireForExternalLaunch();`.

The retire goes AFTER the busy refusal, not before: a refused launch did not win, so it must
not supersede anything. External-over-external stays on the unchanged busy-refusal.

### Deliberately unchanged

* `finishLibretroLaunch` / `finishRetroParkLaunch` retire nothing of their own. Both run
  from inside `launchCtx_`'s continuation (the self-delete trap), the extraction context is
  already consumed by then, and in-app-over-in-app is covered by `open()`'s top.
* `install(em)` supersedes nothing. A Settings-initiated download boots nothing, so there is
  no launch to win.

## Testing

`native/tools/probe_launchcontexts.cpp`, registered in all three places (the
`add_executable` in `native/CMakeLists.txt`, the no-argument runner loop in
`native/tools/run-headless-probes.sh` as `LAUNCHCONTEXTS-OK`, and the `--target` list in
`.github/workflows/ci.yml`). `Qt6::Core` only — no display, no network, no process.

Assertions:

1. A live context delivers its continuation; a retired one does not.
2. `retireForExternalLaunch` drops a pending extraction continuation and a pending
   core-fetch continuation — the bug this exists to prevent.
3. It also drops a continuation whose delivery is ALREADY QUEUED (queued connection, event
   posted, then retire, then `processEvents`). This is the assertion that distinguishes the
   chosen idiom from the deferred one, which would let that delivery through.
4. `takeExtractContext` from inside the continuation leaves the caller intact, and the
   context object outlives the call, dying only on a later event-loop turn.
5. After a consume, `retireForExternalLaunch` is a no-op on the extraction slot — the
   self-delete trap cannot fire.
6. `takeExtractContext` with a stale (already superseded) context leaves the current one
   alone.

Each assertion is mutation-checked: the corresponding line in `LaunchContexts.h` is broken
and the probe confirmed to go red.
