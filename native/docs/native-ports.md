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

Each row shows the game, then `state · upstream · engine · licence · tier`, plus `dump not verified`
where the match rests on the title because the entry publishes no digest.

### Install state — derived, never stored

There is no port-state record anywhere, and there must not be: two records of one fact drift, and the one the
row reads is then the stale one. `src/core/RecompRows.h` recomputes it from four inputs, each with exactly one
owner:

| input | owner |
| --- | --- |
| `installed` | `EmulatorManager::isInstalled` — does the port's binary resolve |
| `libraryMatch` | `recomps::dumpMatch` over the ROM library + the Downloaded list — the ROM-identity gate below |
| `dumpUnverified` | ...and whether that match rested on the title because the entry published no digest |
| `checkingDumps` | a plausible dump is here and its digests are not in the cache yet |
| `installedTag` | `NativePorts::readInstalledTag` — `eb-port-release.txt` inside the install folder |
| `catalogueTag` | the entry's `release.tag` |

```
installed && both tags known && they differ  ->  update available
installed                                    ->  installed
!installed && libraryMatch                   ->  not installed
!installed && checkingDumps                  ->  checking dumps...
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

### The ROM-identity gate (#248, increment b)

`libraryMatch` used to be a title/region match. It is now a **digest** match, and the change is the point: a
recomp is compiled against one exact dump, so a file merely *named* like the game — the PAL disc, a bad rip, a
hack — must not present as a game the user owns.

| the entry publishes | how the row is decided |
| --- | --- |
| any of `crc32` / `md5` / `sha1` / `sha256` | one of those digests must match a dump in the library. A title match alone is **not** a match |
| nothing | the title/region match stands, and the row says **dump not verified** |

`rom_identity.disc_serials` is deliberately **not** counted as a digest: this build has no reader for a disc
image's serial, and an entry that looked gated by one would be gated by nothing at all.

**Hashing is never done at browse time.** `src/core/RecompRows.h` cannot hash — not one function in it opens a
file. It is handed the digests the library's own hash cache already holds (`HashVerify::cachedHashes`, the
same per-path record #97's dump badge writes, keyed on path + mtime + size) and returns `Checking` for a
plausible candidate that has none. `HomeView` then hashes exactly those paths, once each, on the thread pool
(`HashVerify::hashAndCache`), and the section re-derives when each lands. A **warm cache asks for no work at
all**, which is what stops an open section re-hashing a 660 MB disc image on every redraw.

`hashAndCache` writes the four digests into the shared record **without** touching #97's verdict: a file
hashed for a recomp row on a machine with no DAT must not thereby acquire a "we checked and know nothing"
stamp that stops the DAT pass ever running. SHA-256 is computed alongside the other three because no DAT
publishes it but a catalogue entry may, and the alternative is opening a multi-gigabyte file twice.

**Which files are hashed at all** is narrowed first by three facts already in hand — platform, extension
(`rom_extensions`) and byte size (`rom_identity.sizes`), which is RetComM's own scan rule. Two loosenesses are
load-bearing: an **archive** is exempt from the size and extension gates (a `.7z` has its own size and its own
extension, and the cache's digests for it are the digests of the *extracted* stream), and an **unknown** size
is never read as a wrong one.

## The RetComM feed (#248, increment b)

RetComM Launcher publishes its catalogue as a build artefact rather than as part of its program:

```
https://github.com/TechnicallyComputers/retcomm-catalog/releases/latest/download/catalog.zip
```

holding `index.json` (`schema_version`, `titles[]`, `release_tag`, `catalog_date`, and per-platform
`platform_defaults` this build does not read) plus one `titles/<id>.json` per entry, in the same per-title
schema the in-tree catalogue is already written in. `EB_RECOMM_CATALOG_URL` overrides the URL — for a mirror,
and so a live drive can point the feed at a local fixture instead of the real repository.

* **Fetched** by `RecompFeed::refresh()` (`src/core/RecompFeedFetch.cpp`) — one blocking `BoundedFetch::get`
  off the GUI thread, redirects followed, a 4 MB ceiling applied as the bytes arrive, a 20 s deadline, no
  headers and no credentials. At most **once a day** (`recomps/feedCheckedAt` in the portable ini), started
  only when the section is opened, and never on the path that draws it.
* **Cached** at `<data>/recomps/catalog.zip` — its own folder, not `<data>/ports`, which belongs to the user.
  Written through a `.part` file and renamed. The section always draws from this **last good copy**, so
  opening it never waits on a network round trip.
* **A failed fetch changes nothing.** A **broken publish** changes nothing either: bytes that do not parse are
  not written, so one bad release cannot delete the working catalogue on every machine that fetches it.
* **A cached copy that cannot be read is an error row** appended after the real rows (#174), never a section
  that has quietly got shorter. An index that parsed and listed nothing is *not* an error — it is a catalogue
  saying it has nothing, and the in-tree rows still fill the section.

That error row's type is `_recompsfeederror`, **not** `info`, and the distinction is load-bearing on the
themed layout: `HomeView::browseItems` flushes a guidance (`info`) row only when *nothing else* survived the
level, so an `info` row appended after real rows is silently dropped there. A live drive against a
deliberately corrupted cached copy is what found it — the themed section showed the in-tree rows and said
nothing at all about the feed, which is exactly the failure #174 forbids. A `_`-prefixed type is carried
through as an ordinary row on both layouts (the system headers already rely on that), and `activateItem`
refuses it by name.

### Merged by title identity — in-tree wins

Two catalogues describing one game agree on the game and never on the slug (`zelda64recomp` against
`twisted-metal4-psx`), so the **id is not the key**. A feed entry is dropped when an in-tree entry shares its
id, *or* shares its platform **and** any spelling of its game's title — `NativePorts::titleKeys`, the same key
the ROM match is made on, so the catalogue and the ROM gate cannot answer "is this the same game" differently.

The in-tree entry wins because it carries what the published one structurally cannot: `rom_delivery` (the
schema has no field for how a port takes the game file) and a licence that was checked. The same title on
another console is a different game and both rows stand.

### Engine and licence

Every entry in the published catalogue names a recompiler, so every one of them is the **self-compiled** tier.
A published manifest has no licence field — the terms that govern a self-compiled port are the *engine's* — so
the row shows the engine by name and its licence from a small hard table, checked against each project's own
`LICENSE`:

| engine | licence |
| --- | --- |
| `psxrecomp` | PolyForm Noncommercial 1.0.0 |
| `snesrecomp` | PolyForm Noncommercial 1.0.0 |
| `gbarecomp` | PolyForm Noncommercial 1.0.0 |

An engine not in the table shows nothing: a guess about somebody else's terms is worse than silence. **Nothing
of any engine is bundled in this app, and this increment downloads none of it** — the only bytes it fetches
are the catalogue's own JSON. `psxrecomp` is the case #248 named: PolyForm Noncommercial may be *invoked* on a
user's machine and never bundled or redistributed.

A self-compiled row's Install does not start a download, because there is nothing to download — the port is
produced here. It opens its own card (`MainWindow::showSelfCompiledPort`,
`src/ui/MainWindowRecomps.cpp`) naming the engine and its licence, saying that building on this machine
arrives in a later update, and offering the engine's own page.

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

* **(b, part)** the live release lookup that fills `release.tag` for a feed entry, so *update available* can
  fire on one. The feed and the ROM gate themselves are done (above).
* **(c)** the self-compiled tier: toolchain detection (report what is missing and link the official installer;
  never download a compiler), the external build with progress, log tail and cancel, PSX first.
* **(d)** the rebuild-on-update flow: explicit, never automatic, keeping the previous build until the new one
  has launched once.

## Probes

`probe_ports` (`native/tools/probe_ports.cpp`) drives all of it headlessly, QtCore only. Sections 1–11 are
#233's match rails and the embedded-catalogue byte-compare; sections 12–17 are the row model: the state
derivation from fixture inputs, the tier, the grouping and sorting, the error row, the convergence of
`needs ROM` with the game-row verb's gate, and the recorded-tag round trip.

Sections 18–24 are increment (b): the engine/licence table, the feed's parse in its real published shape,
every way a document can be unreadable presenting as a shape error rather than an empty list, the merge, the
gate per digest kind (including a title match with a *wrong* digest, asserted against increment (a)'s own
function so the behaviour change is visible in the probe), the narrowing, the no-hashing-at-browse-time rule,
and the last-good-copy surviving a broken publish **byte for byte**. Fixture catalogues are written with miniz
in the probe process, so each malformed case differs from the good one by exactly the byte it is about.
