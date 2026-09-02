# Native ports and the Recomps section

A **native port** is a static recompilation of one game into a program that runs on this machine: projects
like Zelda64Recomp take a retail N64 ROM, compile it into native code and publish an ordinary GitHub Release.
The user supplies their own copy of the game; nothing containing game code is ever downloaded.

Each upstream is credited **by its own name**. Nothing in this app is branded with the recompilation
toolchain's name — its developers asked a third-party launcher to stop, and this app does not use it either.

Two issues own this feature: **#233** (the pre-built tier — the catalogue, the install, the per-game verb) and
**#248** (the Recomps section, the RetComM feed, and the self-compiled tier that builds a port on the user's
own machine). This document describes what is shipped.

## The catalogue

`native/ports/*.json`, embedded through `native/resources/ports.qrc`, and merged over by any file in
`<data>/ports/*.json` — the same arrangement `<data>/systems` has. The catalogue **rots** (upstreams rename,
re-tag and 404), so it is data a user can correct without a rebuild.

The per-title schema is the RetComM catalogue's own
(`github.com/TechnicallyComputers/retcomm-catalog`, `SCHEMA.md`) so a later increment can read that feed
unchanged rather than translating it: `id`, `name` (the **game**), `kind`, `platform`, `release.github` +
`release.asset_glob.*`, `rom_identity.*`, `rom_extensions`, `install_dir_name`, `launch.*`, `author_notes`,
`notes`, `build.generate.engine`.

Four fields are **ours**, and are marked as ours in `src/core/EmulatorRegistry.h`:

| field | meaning |
| --- | --- |
| `rom_delivery` | how the port takes the ROM: `in_app_menu` (implemented), `beside_exe`, `cli_path` |
| `license` | the port's own licence, shown on the row and on the card before anything is fetched |
| `release.tag` | the release the catalogue calls current — one half of the *update available* comparison |
| *(read-only)* `build.generate.engine` | RetComM's, not ours, but its **presence** is what marks the self-compiled tier |

A port is an `ExternalEmulator` carrying a `NativePortBinding`, so the whole standalone-emulator tier (release
resolution, per-OS artifacts, install into `emulators/<id>/`, launch, process monitoring) runs it with no new
machinery. It lives in its **own** registry (`NativePorts::all()`), never in `EmulatorRegistry::all()`: a port
binds to one **game**, and anything enumerating emulators would otherwise offer Zelda64Recomp on Super Mario
64. `probe_ports` pins that separation as an absence-of-behaviour tripwire.

## The Recomps section (#248, increment a)

`Games → Recomps` — a folder on the games category root, present whenever the catalogue holds an entry. It is
the browse half of the feature: before it, the only way to find a port was to already own the one game it
runs.

The section is a flat list with a **section header per system** (systems in ascending id order; titles sorted
case-insensitively within a system), which is the shape the Live TV channel list already uses and which both
layouts — themed and classic — render without a second code path. A catalogue that cannot be read presents as
an **error row**, never as an empty section: an empty grid would say "there are no recomps", which is a
different statement and a false one.

Each row shows the game, then `state · upstream · licence · tier`.

### Install state — derived, never stored

There is no port-state record anywhere, and there must not be: two records of one fact drift, and the one the
row reads is then the stale one. `src/core/RecompRows.h` recomputes it from four inputs, each with exactly one
owner:

| input | owner |
| --- | --- |
| `installed` | `EmulatorManager::isInstalled` — does the port's binary resolve |
| `libraryMatch` | `NativePorts::matchesRow` over the ROM library + the Downloaded list — **the same gate the game-row verb is offered on** |
| `installedTag` | `NativePorts::readInstalledTag` — `eb-port-release.txt` inside the install folder |
| `catalogueTag` | the entry's `release.tag` |

```
installed && both tags known && they differ  ->  update available
installed                                    ->  installed
!installed && libraryMatch                   ->  not installed
!installed                                   ->  needs ROM
```

Two rules are worth stating because getting either wrong is invisible:

* **An unknown is not a difference.** No recorded tag, or no pinned tag, means nobody knows which release this
  is — that reads as *installed*, never as *update available*. Telling somebody their software is out of date
  on the strength of a fact nobody has is worse than saying nothing. (Tags compare case- and
  whitespace-insensitively for the same reason: `V1.2.2` and `v1.2.2` are one release.)
* **The library decides `needs ROM`, and only that.** A port already installed stays *installed* whether or
  not a matching dump is on this machine; deleting a game must not make an installed program vanish from the
  section.

`installedTag` exists because a port's install folder is the upstream's own zip and no upstream agrees on
where — or whether — it writes a version. `EmulatorManager` records the release tag it resolved at install
time, for native ports only, at the one moment the app knows it. The file lives **inside** the install folder,
so Remove takes it away with everything else.

`libraryMatch` is a title/region match today. The `HashVerify` digest gate is increment (b) and lands behind
this same boolean; the digests are already carried in the catalogue, in HashVerify's own shapes.

### Tier

`pre-built` (a published release binary — every entry this build ships, because N64 has no generic
recompiler) or `self-compiled` (compiled here from the recompiler the entry names). Read off
`build.generate.engine`. Increment (a) acts on the first only; the row model carries the field so increment
(c) adds the second without reshaping anything, and the states `building` / `ready` are reserved in the enum,
unused, for the same reason.

### The verbs

Row activation opens the same card the game row's *Native port* verb opens —
`MainWindow::showNativePort`, one implementation, reached from two places:

* **Install and play** / **Play (native)** — the standalone tier's own install-then-launch. `in_app_menu`
  means the port is launched with no arguments and asks for the game file itself.
* **Open homepage** — the project's own page. Offered whether or not it is installed: it is the only route to
  a port for an OS it publishes no build for.
* **Remove** — deletes the install folder and nothing else. Saves are the port's own, in the port's own
  per-user location, and re-installing picks them up again. Guarded so it can only ever delete
  `<emulators root>/<port id>`.

The card states the licence and, when the port is not installed, that it is an unsigned program this machine's
antivirus may quarantine — Defender's `Bearfoos.A!ml`-class heuristic flags exactly this kind of download, and
saying so beforehand is the difference between an install that looks broken and one that explains itself.

## What is not here yet

* **(b)** the RetComM catalogue consumed as a second feed, merged by title identity, plus the `HashVerify` ROM
  gate and the live release lookup that fills `release.tag`.
* **(c)** the self-compiled tier: toolchain detection (report what is missing and link the official installer;
  never download a compiler), the external build with progress, log tail and cancel, PSX first.
* **(d)** the rebuild-on-update flow: explicit, never automatic, keeping the previous build until the new one
  has launched once.

## Probes

`probe_ports` (`native/tools/probe_ports.cpp`) drives all of it headlessly, QtCore only. Sections 1–11 are
#233's match rails and the embedded-catalogue byte-compare; sections 12–17 are the row model: the state
derivation from fixture inputs, the tier, the grouping and sorting, the error row, the convergence of
`needs ROM` with the game-row verb's gate, and the recorded-tag round trip.
