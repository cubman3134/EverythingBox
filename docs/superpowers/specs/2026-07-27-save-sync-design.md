# Trustworthy Save Sync (roadmap #7) — Design

**Date:** 2026-07-27
**Status:** Draft — approved through brainstorming; awaiting user spec review before plan.
**Origin:** Roadmap #7 ("saves"). The roadmap line carried no description. Brainstorming established that
saves are **already synced** — badly — so this track is not "add save sync" but "make the save sync that
exists trustworthy". This is the deferral `2026-07-23-multidevice-sync-design.md:134-138` parked by name:
*"syncing saves/states per-item (bundle snapshot remains)"*.

## What exists today

**Libretro saves** (`native/src/emu/RetroView.cpp`):
- Save states: 6 slots (`kStateSlots`, `RetroView.h:57`) at `<appdir>/states/<romCompleteBaseName>.state<N>`,
  each with a 240px-wide PNG thumbnail. Raw `retro_serialize` bytes — no compression, no header, no version.
  Measured: one state 1,036,288 bytes, thumbnail 48 KB.
- SRAM: `<appdir>/saves/<romCompleteBaseName>.srm`, written on stop, at exit, and on a ~10 s autosave counter.
- UI is a 6-row slot grid in the pause menu (`showStateSlots`, `:197`) with thumbnail + timestamp. **No
  delete, no rename, no export, no import, no cross-game view.**

**Sync** (`native/src/core/CloudSync.cpp`): a single `everythingbox-sync.zip` carrying `settings.json`,
`addons/`, `themes/`, **`saves/` and `states/`** (`:491-492`). Applied wholesale — `applyBundle` (`:502`)
overwrites every local file — and the startup policy is literally *always take the cloud*
(`main.cpp:69-73`), run before settings are even read.

## The four problems

1. **No merge, and no conflict rule.** Whole-file last-writer-wins, cloud-always-wins at startup. Play the
   same game on two devices and one side's saves are gone with no notice and no copy.
2. **Any save write re-uploads everything.** `stateHash()` (`:546-581`) folds a per-file SHA-256 of every
   file under `saves/` and `states/` into one global fingerprint, so a single F2 press invalidates the whole
   bundle. Six slots × ~1 MB across a library pushes that into hundreds of MB per sync.
3. **Some saves are never synced at all.** `LibretroCore::saveDir` defaults to `"."` (`LibretroCore.h:105`)
   and is **never assigned** — only `systemDir` is. So `RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY` hands cores the
   process CWD, and cores that write their own save files put them in the app directory. The deployed
   install has `*.smpc` files loose in `C:\EverythingBox-app\`; the repo root has `4Mbit_cart.brm`, `scd_U.brm`.
   None are under `saves/`, so none are backed up.
4. **Saves are keyed to an opaque, collidable name.** The key is the ROM's `completeBaseName`. Remote ROMs
   are cached under 40-hex hashed filenames, so most real `.srm` files are named `101306d4…` for a ROM that
   may no longer exist. Two systems with the same base name collide on one file.

## Design

### 1. `SaveSyncPlan` — the pure decision layer (`native/src/core/SaveSyncPlan.{h,cpp}`)

All of the "what should happen to this file" logic, with no network and no filesystem, so `probe_savesync`
can drive every row of the decision table as data.

```cpp
namespace SaveSyncPlan
{
    // One file's state on one side. mtimeMs is epoch ms; sha is content, empty when absent.
    struct Entry
    {
        QString name;      // "Zelda.srm" / "Zelda.state1" / "Zelda.state1.png"
        QString sha;       // empty = the file is not present on this side
        qint64  mtimeMs = 0;
        qint64  size = 0;
    };

    enum class Act
    {
        None,           // already in step
        Upload,         // local is the newer/only copy
        Download,       // remote is the newer/only copy
        Conflict,       // both changed since the baseline and differ — newest wins, loser PRESERVED
        DeleteRemote,   // deleted locally, with a tombstone to prove it
        DeleteLocal     // deleted remotely, with a tombstone to prove it
    };

    struct Decision
    {
        QString name;
        Act     act = Act::None;
        bool    localWins = false;   // Conflict only: which copy stays under the real name
        QString reason;              // one line, for the log — never a silent action
    };

    // The whole rule, as one pure function over three views + the tombstone set.
    // `firstRun` (no baseline at all) is a HARD no-delete mode: see the safety property below.
    QVector<Decision> plan(const QHash<QString, Entry>& local,
                           const QHash<QString, Entry>& remote,
                           const QHash<QString, Entry>& baseline,
                           const QSet<QString>&         tombstones,
                           bool                         firstRun);

    // The name a losing copy is preserved under: "<base>.conflict-<deviceId>-<yyyyMMdd-HHmmss><ext>".
    QString conflictName(const QString& name, const QString& deviceId, qint64 mtimeMs);

    // A .conflict-* artifact is local-only recovery and must never enter the synced set — syncing it would
    // multiply one conflict across every device.
    bool isConflictArtifact(const QString& name);
}
```

**The decision table** (`name ∈ local ∪ remote ∪ baseline`):

| local vs baseline | remote vs baseline | Act |
|---|---|---|
| unchanged | changed | `Download` |
| changed | unchanged | `Upload` |
| changed | changed, contents differ | `Conflict` |
| changed | changed, contents identical | `None` (both arrived at the same bytes) |
| absent, tombstoned | present | `DeleteRemote` |
| present | absent, tombstoned | `DeleteLocal` |
| absent, **no** tombstone | present | `Download` (a missing file with no tombstone is a restore, not a delete) |
| present | absent, **no** tombstone | `Upload` |
| **`firstRun`** | anything | `Upload` local, `Download` remote-only, **never any Delete** |

**Conflict resolution.** A save state is opaque binary; there is no merge. Newest `mtimeMs` wins the real
name. **The loser is never deleted** — it is written as `conflictName(...)` and reported. "Two devices played
the same game" must cost a rename, not a save.

**Preserving the loser is asymmetric, and the remote case is the one that can go wrong.** If the *local*
copy loses, its bytes are already on this disk: rename it, then download the winner. If the *remote* copy
loses, its bytes exist **only in the cloud** and we are about to overwrite that name with ours — so the
transport must **download the losing remote copy first**, write it as `.conflict-…`, and only then upload the
winner. A conflict that uploads before preserving is indistinguishable from the data loss this track exists
to remove. `SaveSyncPlan` therefore emits `Conflict` with `localWins` set, and the transport's ordering for
`localWins == true` is: fetch-loser → write `.conflict-…` → upload winner.

**A name present in the baseline but absent from both sides** is simply dropped from the baseline: nothing to
upload, nothing to download, nothing to delete. It is listed here because "in baseline, gone everywhere" is
the row a table like this usually forgets, and treating it as a deletion would let a stale baseline delete a
file that no longer exists anyway.

**Clock skew.** Newest-wins is only as good as two clocks. Within `kSkewWindowMs = 5000` the mtimes are
treated as tied, and ties break **deterministically**: greater content `sha` first, then greater `deviceId`.
Both devices must independently compute the *same* winner — a rule where each side thinks it won is worse
than no rule.

### 2. `SaveSync` — the transport (`native/src/core/SaveSync.{h,cpp}`)

Drives `SaveSyncPlan` against Drive using `CloudSync`'s existing primitives (find-or-create folder, upload,
download, metadata). One Drive file per local file, in a `saves/` subfolder of the app folder.

- **Remote manifest** `saves-index.json`: `{name, sha, mtimeMs, size, deviceId}` per file.
- **Local baseline** at `AppPaths::dataDir()/save-baseline.json`: what this device last successfully synced.
  This is what makes "did *I* change it or did *they*" a per-file question instead of one global fingerprint.
- **Tombstones**: reuse the existing `Tombstones` mechanism from the progress-sync path rather than inventing
  a second one.
- **Upload is per-file and debounced**, so an F2 press sends one state. Exit flushes anything pending.
- **Torn-write guard**: hash → upload → re-hash. If the file changed underneath, abandon that upload and
  catch it next pass. Never publish a half-written state.

### 3. The bundle stops carrying saves

`saves/` and `states/` are removed from `buildBundle` (`CloudSync.cpp:491-492`), from `applyBundle`, **and
from `stateHash()`** (`:564-581`). That last one is the point: it is what stops one save write re-uploading
addons, themes and settings along with it.

### 4. `saveDir` and the stray-file sweep

`LibretroCore::saveDir` is assigned the real `<appdir>/saves/` before core load, beside the existing
`systemDir` assignment (`RetroView.cpp:703`). A **one-time sweep** moves already-stray core save files from
the app directory into `saves/`. The sweep only moves names matching a known core-save extension set
(`.srm .sav .brm .smpc .mcd .mcr .eep .fla .state`), skips anything currently open, and logs every move — it
must never scoop up an unrelated file from the install directory.

### 5. The sidecar index — making a save identifiable

`AppPaths::dataDir()/saves-meta.json`: per save file, `{title, system, romPath, updatedAt}`, written when a
save or state is written. This is what turns `101306d4….srm` back into "Zelda II (NES)", survives the ROM
being deleted, and lets a conflict notice say *which game* collided instead of quoting a hash.

**Existing files are not renamed.** Renaming saves under a user is exactly the kind of irreversible tidying
this design refuses; the sidecar is additive, and any save without an entry simply displays its filename.
**New** saves are namespaced by system to stop cross-system collisions going forward.

## Data flow

```
startup ─→ pull saves-index.json ─┐
          scan saves/ + states/  ─┼─→ SaveSyncPlan::plan(local, remote, baseline, tombstones, firstRun)
          read save-baseline.json ┘         │
                                            ├─ Upload      → per-file PUT, then update baseline
                                            ├─ Download    → write local, then update baseline
                                            ├─ Conflict    → keep winner under the real name,
                                            │                write loser as .conflict-<device>-<ts>,
                                            │                notify naming the GAME (via saves-meta.json)
                                            └─ Delete*     → only ever with a tombstone

save written (F2 / SRAM autosave) ─→ mark dirty ─→ debounce ─→ upload that ONE file
exit ─→ flush pending uploads (bounded by the existing 8 s watchdog)
```

## Safety property (a rule, not a behaviour)

**The first sync after this upgrade never deletes anything.** Saves currently live inside the cloud zip;
there is no per-file baseline yet. On the first run the plan is computed with `firstRun = true`, which
disables every `Delete*` outcome: local files upload, cloud-only files download, and a cloud file with no
local counterpart is *not mine to delete* until a real tombstone exists. Getting this wrong once is
unrecoverable, so it is asserted directly and mutation-tested, not left to reviewer attention.

## Error / edge handling

| Situation | Behaviour |
|---|---|
| Two devices edited the same save | Newest wins the real name; **loser preserved** as `.conflict-…`; the user is told, by game name |
| Clocks disagree | Within `kSkewWindowMs` (5 s) mtimes tie; ties break deterministically on sha then deviceId, so both devices agree |
| A save is written mid-upload | Hash/upload/re-hash mismatch ⇒ abandon and retry next pass. Never a torn state |
| `.conflict-*` artifacts | Excluded from the synced set entirely — local recovery only |
| Offline / Drive quota / auth failure | Queue and retry; a failed upload never touches the local file |
| Local file deleted by the user | Tombstone ⇒ `DeleteRemote`. Without a tombstone, a missing file is a **restore**, never a delete |
| Cloud file vanished (another device deleted it) | `DeleteLocal` only with a tombstone; otherwise re-upload |
| First run after upgrade | `firstRun` mode — no deletions of any kind, either direction |
| Stray core save in the app dir | One-time sweep into `saves/`; extension-allowlisted, skips open files, every move logged |
| A save with no sidecar entry | Displays its filename. Never blocks sync |
| Two systems, same ROM base name | New saves namespaced by system; existing files left alone and disambiguated by the sidecar |
| A state slot's `.png` thumbnail | Syncs as an ordinary file; a missing thumbnail is cosmetic, never an error |
| In the baseline, absent from both sides | Dropped from the baseline. Not a deletion — there is nothing to delete |
| Conflict where the **remote** copy loses | Its bytes exist only in the cloud: **download the loser first**, write `.conflict-…`, then upload the winner. Never the other order |

## Verification

- **`probe_savesync`** (new, pure, RED-first, sentinel `SAVESYNC-OK`):
  - Every row of the decision table, both directions.
  - **`firstRun` produces no `Delete*` for any input** — mutation-tested, because its failure mode is
    unrecoverable data loss.
  - Conflict: winner selection by mtime; `conflictName` shape; the loser always present in the plan; and
    `localWins` set correctly in both directions, since it is what tells the transport whether it must fetch
    the losing copy before overwriting it.
  - A name in the baseline but absent from both sides yields **no** decision at all — not a delete.
  - Clock skew: two entries inside `kSkewWindowMs` resolve identically when the inputs are swapped
    (device A's view and device B's view must agree).
  - Tombstones: delete round-trip, and that a **missing file without a tombstone is a restore, not a delete**.
  - `isConflictArtifact` excludes `.conflict-*` from the synced set.
- **Live, single device:** save → sync → confirm the Drive `saves/` folder holds one file per save and the
  bundle zip no longer contains `saves/`/`states/`; confirm one F2 press uploads one file rather than
  re-uploading the bundle; confirm the stray `.smpc` files move into `saves/` and then sync.
- **Live, conflict path without a race:** write a file on the device, rewind its mtime, hand-edit the
  baseline to simulate a divergent remote, sync, and confirm the winner is correct **and the loser exists on
  disk**.
- **Genuinely two-device (user-gated):** save on A, sync, confirm on B; then save on both while offline and
  confirm one wins and the other survives. Recorded as user-gated if a second machine is unavailable —
  never claimed as passed on inference.
- Suite + app compile. No perf run: sync is off the render path, and the change strictly *reduces* work.

## Non-goals

- **The 17 standalone emulators.** `ExternalEmulator` (`EmulatorRegistry.h:10-38`) has no save-path field of
  any kind, and per-emulator save locations vary by platform and install mode. That is its own track.
- **A save-management UI** — browse/rename/delete/export/import. The sidecar index built here is the
  groundwork for it, but the screen is not in this track.
- **The netplay save/load desync** (`RetroView.cpp` never guards `saveState`/`loadState` against
  `netActive_`, so F4 mid-session desyncs silently). A real bug, recorded, and a different one from sync.
- Compressing save states, changing the slot count, or altering the pause-menu slot grid.
- Any change to the progress-sync document (`everythingbox-progress.json`) or its merge rules.
