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
| `building` / `builtNotLaunched` | a local compile is running, or one finished and its program has not run yet |
| `updateAvailable` | `recompupdate::compareBuild` — the self-compiled tier's update, from the build's own stamp |
| `previousKept` | the build that worked before the last rebuild is still on disk (see *Updates*) |

```
building                                     ->  building...
installed && updateAvailable                 ->  update available
installed && builtNotLaunched                ->  built - ready to play
installed && both tags known && they differ  ->  update available
installed                                    ->  installed
!installed && libraryMatch                   ->  not installed
!installed && checkingDumps                  ->  checking dumps...
!installed                                   ->  needs ROM
```

`updateAvailable` outranks *built - ready to play* deliberately: a copy that was built and never launched can
still be out of date — the catalogue moves on its own schedule, not the user's — and *ready to play* would
hide the one fact on the row with a button attached to it.

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

## The self-compile tier (#248, increment c)

A catalogue entry that names a recompiler (`build.generate.engine`) ships **no binary anywhere**. The port is
produced on the user's own computer: the recompiler is run against the dump they already own, and the C++ it
emits is compiled by their own toolchain. That is the half the issue was actually opened for.

### The toolchain: detected, never installed

`core/Toolchain.h` is a **pure decision table** — (what was found on this machine, which OS) → (a verdict and
the two sentences a person reads). Four verdicts, and every one of them carries both sentences, so a state
with a verdict and no explanation is unreachable:

| compiler | CMake | verdict | what the card says |
|---|---|---|---|
| yes | yes | `Ready` | which compiler and which CMake it found; a **Build it on this computer** verb appears |
| yes | no  | `NoCMake` | names the compiler it found, and that CMake is what is missing |
| no  | yes | `NoCompiler` | there is nothing for CMake to compile with |
| no  | no  | `NoToolchain` | both, named, with both installer pages |

The compiler preferred per OS is a property of where the system headers come from, not a taste: **MSVC** first
on Windows (it is the only one of the three that arrives with the Windows SDK — a bare `clang` on `PATH` very
often has no MSVC headers behind it and dies at the first `#include <windows.h>`), **clang** first on macOS,
**gcc** first on Linux. A program that is present but did not answer `--version` in a shape we parse still
counts as present.

`Toolchain.cpp` is the only half that looks at the machine: `vswhere -latest -products * -requires
…VC.Tools.x86.x64` for MSVC (so a Visual Studio with only the .NET workload is correctly *not* a C++
compiler), `cl` / `clang++` / `g++` / `cmake` on `PATH` plus the directories a Windows installer uses when the
user did not tick "add to PATH". The answer is cached in `<data>/recomps/toolchain.json` for a week and the
card offers **Check again**, so somebody who installs the Build Tools in the other window does not have to
restart the app to be believed. A cache file that cannot be parsed re-probes; it can never assert that this
machine has no compiler.

**This app does not download or install a compiler, an SDK or a toolchain pack, and it has none bundled to
fall back on.** That is a deliberate divergence from RetComM, whose recipe downloads a `cmake-clang-v1` pack
(`build.toolchain`); we read that field and never act on it. A compiler is a large, long-lived, system-wide
thing a person is entitled to choose, place and update themselves.

### The build

`core/RecompBuild.h` is the model and it is pure: the state machine, the step plan, the log tail, the progress
line, the failure sentences and the path safety. `RecompBuildRunner` is the only thing that starts a child
process. `RecompBuildJob` is the orchestration, lives outside the UI, and runs on a pooled thread.

    Idle -> FetchingSource -> Unpacking -> Generating -> Configuring -> Compiling -> Staging -> Succeeded
                                       \-> Failed        \-> Cancelled       \-> Blocked

1. **the toolchain**, first, before a byte is downloaded — no compiler or no CMake is `Blocked`, not `Failed`,
   and nothing has been written;
2. **the source**, from the project's own GitHub zipball (`build.source.github` / `.ref`), bounded and
   deadlined;
3. **unpacked** into `<data>/recomps/builds/<id>/source`, refusing any member that is absolute, drive-lettered
   or holds a `..` segment;
4. **the recompiler**, harvested out of that tree by the name the entry gives it, run against the user's dump
   — `psxrecomp <config> --disc <path>`, which is what SCHEMA.md specifies for PSX. **PSX only**: the SNES and
   GBA generators take arguments that document does not spell out, and an entry for one of those is refused
   with a sentence rather than built with invented flags;
5. **the user's own cmake**, to configure and then to compile;
6. **staged** into `emulators/<id>/`, which is what makes `EmulatorManager::isInstalled` true and lights up
   the existing *Play (native)* verb. One launch path, not two.

The build **survives navigating away**: it belongs to `RecompBuildJob`, not to the card that started it, so
walking back out leaves it running with a sticky note and a live Recomps row (`compiling 62% · 3m 14s ·
[ 62%] Building CXX object …`). Opening the row again shows the same build. **Cancel actually stops it** — a
flag the runner watches between polls, `terminate()` then `kill()` — and the one rule the whole thing rests on
is that **a terminal state absorbs everything**: the child's non-zero exit arrives *after* the cancel, and a
machine that took it at face value would rewrite the user's own cancellation as "the compiler failed with
code 1".

Only one build at a time. Two would compete for every core on the machine and make both slower; a second Start
is refused with a sentence naming the one that is running.

### Failures read as sentences

Never a spinner that stopped. Every terminal state carries a sentence, and the ones that ran a program carry
the path to the full log (`<data>/recomps/builds/<id>/build.log`, capped at 32 MB with one line saying so):

* `cmake --build exited with code 1. The full log is at …`
* `psxrecomp ended unexpectedly — it wasn't asked to stop and it didn't finish. The full log is at …`
* the source failed to download / was over the ceiling / would not unpack — each says **nothing was changed**;
* **the artefact-missing case**: every step exited 0 and the program is not there. On Windows that is exactly
  what Defender quarantining a freshly compiled unsigned exe looks like from here, so the sentence names the
  path, says the program was unsigned and brand new, and points at Windows Security → Protection history. It
  never suggests disabling anything and never silently retries.

### The engine, and why fetching one is not redistribution

`psxrecomp` is **PolyForm Noncommercial 1.0.0**: it may be *run* by the person who obtained it and may not be
redistributed by us. So:

* **nothing engine-shaped is in this repository**, has ever been committed to it, or is in the app's shipped
  artefact. The only "recompiler" in the tree is `tools/stub_recomp_engine.cpp`, a stand-in the probe drives;
* an engine reaches a machine only as part of the **source the user's own machine fetched** from the project's
  own release, at the moment that user asked for a build. It lands under `<data>/recomps/builds/<id>/source`
  (or `<data>/recomps/engines/<engine>` when a separate tools pack is used), which is a per-machine directory
  the app creates after installation — never inside the repository and never in the artefact anybody
  downloads from us;
* `writeEngineNotice` puts a `WHERE-THIS-CAME-FROM.txt` beside it naming the engine, its licence and the URL
  it came from, so those bytes carry their provenance even to somebody who finds the folder later;
* the licence is on the row and on the card **before** anything is fetched.

### No ROM moves

The dump is passed to the recompiler as one command-line argument and that is the entire relationship between
this feature and somebody's game file. No step in any plan copies it, no plan's working directory is the
folder it lives in, and the probe asserts both — and asserts that after a full run the fixture dump is
byte-for-byte what it was and that no copy of its bytes exists anywhere the build wrote.

## Updates (#248, increment d)

A recomp built here can go out of date, and rebuilding one used to mean destroying the copy that worked.
`src/core/RecompUpdates.h` is the whole of the answer; `RecompUpdates.cpp` is the handful of functions that
move directories.

### What a build records about itself

Every successful build writes `eb-recomp-build.json` into its own install folder — beside the program it
describes, so Remove takes it away with everything else, exactly as `eb-port-release.txt` works for the
pre-built tier. It holds the **engine version** (`build.sdk.version`, or `build.generate.engine_version`
where an entry spells it that way) and the **recipe**: engine, source repo, source ref, SDK id, generate
config and out dir, cmake dir, target and config. It also holds `built_at_ms` and `launched`.

`built_at_ms` is written for a person reading the file and is **read by nothing**. That is the point: a
catalogue is republished whenever anybody's submission is approved, so a build is not out of date because
somebody else's entry changed. The feed's own `release_tag` and `catalog_date` are not in the stamp at all.

### The comparison

| the catalogue says | verdict | row |
| --- | --- | --- |
| a higher engine version | `EngineNewer` | update available |
| a different engine version that cannot be ordered (a date, a codename) | `EngineChanged` | update available |
| the same engine, a changed recipe field | `RecipeChanged` | update available |
| the same entry, republished | `UpToDate` | installed / ready |
| an **older** engine version | `CatalogueBehind` | *not* an update — and the card says why |
| nothing (no stamp) | `Unknown` | installed — never "out of date" |

Two rules carry it, and both are the rule the rest of this feature already runs on:

* **An unknown is not a difference.** A field either side leaves empty is compared with nothing. That is also
  why the comparison walks the recipe **field by field** instead of hashing it: a digest would mean the first
  release of this app to add a tenth recipe field declared every recomp on every machine out of date on
  upgrade day.
* **A catalogue that went backwards is not an update.** Releases get yanked and pins revert; presenting that
  as an update spends twenty minutes of somebody's processor going downhill.

### A rebuild is explicit

Nothing starts a build because a catalogue changed. The row's label moves and the card offers *Rebuild it with
the newer version*; a person presses it. `probe_recompbuild` asserts that structurally as well as
behaviourally: the feed's own translation units do not so much as name `RecompBuildJob`, and the single call
that starts a build is in `MainWindowRecomps.cpp`'s verb switch and nowhere else under `src/`.

### The previous build is kept until the new one runs

This is the safety property of the increment: **a rebuild that produces something broken must never have
destroyed the build that worked.**

A rebuild **moves** the installed copy to `<emulators>/.eb-previous/<id>` — a rename, not a copy, so it is
atomic and cannot half-succeed, and a sibling folder so it is guaranteed to be on the same volume. The new
build is staged into the empty place. If the installed copy cannot be moved (it is running, a file is locked)
the rebuild is **refused** before anything is staged, rather than built over something that cannot be put
back.

From then on:

* the new program **runs** → the stamp's `launched` is set and the kept copy is removed. "Ran" means a
  process existed and either the user closed it themselves or it stayed up for four seconds —
  `GameLauncher`'s own threshold for *closed immediately = a failed boot*, reused rather than re-chosen. The
  judgement is conservative on purpose: keeping a dead copy costs disk, dropping a live one costs somebody
  their working program;
* the new program **does not run** → nothing is dropped. The row offers *Go back to the previous build*,
  which removes the replacement and renames the kept copy back. Its stamp travels with the folder, so the row
  returns to saying exactly what it said before the rebuild;
* the rebuild **fails** → nothing changed. Every failure before staging never reached the move at all; the
  one window where it did (the compile worked, the artefact was not there — the Defender case) restores the
  previous build immediately.

### The disk cost

Two builds of one title are on the machine between a rebuild and its first run, and that is said rather than
discovered: the row carries *previous build kept* beside its state, and the card and the build's own ending
sentence give the size and the path. Only **one** kept copy per title ever exists — `keepAside` drops the
older one before making a new one, so five rebuilds do not leave five dead builds behind.

## What is not here yet

* **(b, part)** the live release lookup that fills `release.tag` for a feed entry, so the PRE-BUILT tier's tag
  comparison can fire on one. The feed and the ROM gate themselves are done (above), and the self-compiled
  tier's update does not depend on it.
* **(c, part)** SNES and GBA generate recipes (PSX is wired up; the other two are refused with a sentence),
  and a separately downloaded `build.sdk` tools pack — today the recompiler is harvested from the source tree,
  which is what SCHEMA.md says is preferred.
* **the compile-to-gameplay leg**, end to end, against a real `psxrecomp` and a real dump. There are no ROM
  files on the machine this was written on and none was obtained, so every layer around the build is driven
  and probe-covered while the build itself has only ever run against the in-tree stub engine.

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

Section 25 is increment (d)'s row half: the order the derivation reads its inputs in (an update outranking
*ready*, a build in flight outranking both, the pre-built tag comparison untouched), the kept-copy qualifier
reaching the row, the catalogue adapter — including a republish that changes the name, the licence and the
release tag and still compares as up to date — and the two spellings of the engine version off real catalogue
documents.

`probe_recompbuild` (`native/tools/probe_recompbuild.cpp`) is increment (c)'s, and it is a separate target
because its subject is a **child process** rather than a document: it drives the toolchain decision table over
all sixteen combinations of (MSVC, clang, gcc, CMake) on all three operating systems, the state machine's whole
path, the failure sentences, the log tail against a hundred thousand lines and against one line of forty
thousand characters, the path safety, and then the runner itself against the in-tree stub engine — exiting
cleanly, exiting non-zero, dying rather than exiting, being **cancelled mid-run** (deterministically: the flag
is set from the line callback on the child's third line), and finishing successfully while leaving no artefact
behind. It also asserts that the fixture dump it points the plan at is untouched afterwards, that no copy of
its bytes reached the workspace, and that this repository holds no engine binary.

Section 13 is increment (d): version ordering, the stamp's round trip and its refusal of a schema from the
future, the comparison case by case (newer engine, changed recipe, an unchanged entry republished, a
catalogue that went backwards, an unknown on either side), what proves a build, and then the
keep-until-launched rule **on real directories** — a rebuild that runs (the old copy goes, exactly one build
is left), a rebuild that never starts (the old copy is still there, whole, and still a program, and going
back puts it where the launcher looks), a rebuild that starts and dies in 300 ms (also not a launch), a
failed staging restoring the previous build byte for byte, a restore with nothing kept leaving the install
untouched, five rebuilds leaving one kept copy, and two hundred wildly different catalogues compared against
the stamp without writing a byte or starting anything.
