# Game updates and DLC

PS3, Switch, Wii U, 3DS and Vita games ship as **base + update + DLC**, and every one of those emulators has
its own way of taking that content in. Left to itself, a frontend hands the emulator the base game and the
player is expected to open the emulator anyway to install the rest — which is the one thing a frontend exists
to stop.

So EverythingBox installs it for you, before the emulator boots.

Issue **#189** owns this feature. This document describes what increment 1 ships.

## Where the files go: the sidecar convention

Put the packages in an `updates/` and a `dlc/` folder **beside the game file**:

```
ROMs/
  Wii U/
    Some Game [00050000101C9300].wux
    updates/
      Some Game v16/          <- a folder (Cemu update content is code/ content/ meta/)
    dlc/
      Some Game DLC/
  Switch/
    Great Game [0100000000010000].nsp
    updates/
      Great Game [0100000000010800][v65536].nsp
    dlc/
      Great Game DLC 1.nsp
```

Each **direct child** of `updates/` or `dlc/` is one package — a single file, or a folder. Nothing is recursed
into: one child, one package. The folders sit next to the game, so they travel with it and a game that has no
extra content simply has no folders (and nothing is even looked at on launch).

EverythingBox does not **acquire** updates or DLC. These are your own files. Console keys and firmware are
always yours to supply too, and no game file is ever modified.

## The title id

Most of these emulators address content by the game's **title id**, so it has to be derivable:

1. A `titleid.txt` beside the game — one line, e.g. `00050000101C9300`. This wins over everything and is the
   escape hatch for a dump whose name says nothing.
2. The game file's own name: 16 hex digits (Switch / Wii U, usually in `[brackets]`) or a Sony serial like
   `BCUS98148` (PS3 / Vita).
3. The game's folder name, read the same way.

If none of those says anything, the install is **skipped and reported** — never guessed. A wrong title id
would write into another game's content store.

## Per-emulator status

The rules live as **data** in the emulator registry (the same JSON schema a `<data>/emulators/*.json` file of
your own can write), not as per-emulator code. Four recipe kinds cover every case:

| Emulator | Recipe kind | What happens | Status |
|---|---|---|---|
| **Ryujinx / Ryubing** (Switch) | `jsonRegistry` | The per-title `games/<titleId>/updates.json` and `dlc.json` indexes are edited in place | **Wired** |
| **Cemu** (Wii U) | `copyTree` | Content is copied into `mlc01/usr/title/0005000E/<low>` (update) and `.../0005000C/<low>` (DLC) | **Wired** |
| **RPCS3** (PS3) | `cli` (`--headless --installpkg`) | Described by the recipe; performed by the existing PS3 update chain, which also fetches Sony's official update PKGs and the firmware | **Described** |
| **Vita3K** (Vita) | `copyTree` | `ux0/patch/<titleId>` and `ux0/addcont/<titleId>` | **Written, not wired** |
| **Azahar / Citra** (3DS) | `emulatorUpdater` | The emulator's own *File ▸ Install CIA* flow. Its content store is keyed by that console dump's `movable.sed`, so no path can be computed and there is no CLI install | **Written, not wired** |

A recipe kind a build does not recognise is **ignored with a single logged line** — never an error, never a
crash. A registry file from a newer build degrades; it does not break the launch of a game that needs none of it.

## What it will not do

Three rules, and none of them has an override:

* **It never replaces content you installed yourself.** A file already at the destination with different bytes
  is left exactly as it is and reported; the game launches with it. The only thing it will overwrite is a file
  its own record says it put there.
* **It never moves a choice you made in the emulator.** Ryujinx's "which update is selected" pin is yours; it
  is only ever written when it is unset or when it still points at a package this app installed.
* **It never blocks a launch.** Every failure is reported on the same surface a missing BIOS uses, and the base
  game boots.

Before the **first** write for a game, the emulator's content index is snapshotted into the install record —
the registry file's bytes, or the destination folder's listing — so what was there before this app touched it
stays stateable. The snapshot is taken once and never retaken.

## Idempotence, and the install record

Installing is tracked per (emulator, title id, package) in a small device-local record under
`<data>/contentinstall/`. It holds each package's SHA-1, a size+modified-time stamp, and where it went.

* A launch installs only what the record does not already claim, so a 4 GB package is copied **once**, not on
  every launch.
* The stamp is the fast gate — a package whose size and time still match is skipped without reading a byte.
  The hash is the authority, so a package that was merely touched, moved or renamed is recognised.
* **The record is authoritative in both directions.** A package it has recorded is not reinstalled even if it
  is no longer at its destination, because "it is gone" is usually *you* having removed it in the emulator's
  own UI. Deleting `<data>/contentinstall/` is what asks for a fresh install.

## Per-game control

Two levers, on the game's own settings — reachable from the themed detail view's **Launch options…** and from
the **Start ▸ Emulation settings** panel (which is how the classic grid reaches per-game settings):

* **Game updates** — *Newest available* (the default), *Don't install any update*, or a **pinned version**.
  The pin is free text, matched case-insensitively against the package's file name, so `v65536` or `1.0.3`
  both work without the app having to understand any vendor's versioning. "None" is a real answer: speedruns,
  mod setups, and "that patch broke it" are all real.
* **Downloadable content** — on (the default) or off.

Both are stored in the same per-game override record as the rest of a game's launch options, so they sync with
your other per-game settings. Clearing a game's launch options clears them too.

A master switch — **Install game updates and DLC before launch** — lives in Settings ▸ General for anyone who
drives their emulators' content stores entirely by hand.

## What is not here yet

* **Server-served attachments.** Increment 1 sources from the sidecar folders only. Content served alongside a
  game by EverythingBoxServer feeds the same plan in a later increment.
* **Vita3K and Azahar** carry recipes but nothing acts on them yet.
* **The install runs on the launch thread.** It reads no bytes at all unless something is genuinely new, but a
  multi-gigabyte *first* install will hold the launch while it copies.
* **Real emulator ingestion is unverified.** The mechanics are driven against fixture data directories in
  `probe_contentinstall`; no Switch or Wii U game exists on the machine this was built on. Confirming that
  Ryujinx and Cemu actually *read* what was installed needs real content and real hardware.
