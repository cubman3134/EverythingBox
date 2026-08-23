# Contributing to EverythingBox

Thanks for wanting to help. This is a Qt 6 / C++17 desktop-and-TV app with an
unusual amount of load-bearing convention in it — the sections below are the
things that will actually get a pull request rejected, so they are worth the
five minutes before you write code.

## Building

Everything lives under `native/`. Point your shell at Qt and libmpv first:

```bash
export PATH="/c/Qt/6.8.3/msvc2022_64/bin:/c/mpv-dev:$PATH"
```

Configure once. The app is gated behind an option, because the libretro
frontend and its `probe_core` harness build with nothing but CMake and a C++17
compiler — the Qt/libmpv dependency only exists for the app:

```bash
cmake -S native -B build -DEVERYTHINGBOX_BUILD_APP=ON \
  -DCMAKE_PREFIX_PATH="C:/Qt/6.8.3/msvc2022_64" \
  -DMPV_INCLUDE_DIR="C:/mpv-dev/include" -DMPV_LIBRARY="C:/mpv-dev/libmpv.lib"
```

Then build **named targets only**:

```bash
cmake --build build --config Release --target everythingbox
cmake --build build --config Release --target probe_nav probe_meta
```

`--config Release` matters because the Windows generator is multi-config; the
probe runner looks for executables in `build/` *and* `build/Release/`, so either
config works, but a Release-configured tree with a Debug-built probe will look
like "not built" to the suite.

**Never run a target-less `cmake --build build`.** `native/CMakeLists.txt`
declares 52 probe harnesses in addition to the app, and the default target
builds all of them. That is many minutes of compiling you almost certainly did
not want. Name what you need.

SDL2 (gamepad input) is optional at configure time — without it the app builds
fine and controller input is simply absent. Release binaries do link it; see
`.github/workflows/release.yml`.

### Install the git tooling (once per clone)

The repo carries two git-side pieces that are **not** active on a fresh clone until
you install them:

```bash
bash native/tools/install-git-hooks.sh
```

That installs the `pre-commit` version-bump hook and registers the `ebversion` merge
driver (`git config merge.ebversion.*`). The hook bumps the patch version on every
ordinary commit; the driver auto-resolves the version lines in `native/CMakeLists.txt`
and `native/src/main.cpp` to the **higher** of the two sides on a merge, so the version
bump every branch carries no longer conflicts on every merge (issue #181). Both are
per-clone git config, so a fresh clone must run the script again. It is idempotent and
covers every linked worktree of the clone in one run.

## The gate

Before opening a pull request:

```bash
BUILD_DIR=build bash native/tools/run-headless-probes.sh
```

It must end with:

```
ALL HEADLESS PROBES PASSED
```

Anything else is a failing branch, no matter how unrelated the failure looks.
The suite needs no display, no GPU and no ROMs — the windowed probes run under
Qt's `offscreen` platform plugin, and the build drops a `qt.conf` beside the
probe executables so they find Qt's plugins without you exporting
`QT_PLUGIN_PATH`. CI runs the same script on every push and pull request
(`.github/workflows/ci.yml`).

The suite is not only probe executables. It also contains source-level gates
that scan the tree: the QML no-direct-selection-writes gate, the RetroView
`.srm` path gate, the probe data-dir isolation wiring gate, the bundled-theme /
registry drift gate below, the Appearance theme-gallery reachability gate, the
themed local-leaf routing parity gate, the
mutation-driver rule, and the old-brand gate. Those fail on code you wrote even
if every probe binary passes. One more gate is a property of the run rather than
of the source: `exe-folder contamination` compares the build folder's app-data
footprint before and after the suite, and fails if anything changed it while the
suite ran — normally a probe, occasionally an app or a build running out of that
same folder.

## Driving the app live (the uitest channel)

The probe suite cannot see timing, focus, or what a user actually experiences.
Anything of that shape — a delay, a focus hand-off, a screen that renders but is
unreachable — has to be driven in the real app before it merges. That is what
this channel is for, and it needs no foreground and no OS focus, so it works
while you keep using the machine.

**Never point any of this at `C:\EverythingBox-app` (or whatever the installed
copy is on your machine).** It is somebody's real library, and the channel can
delete things through the UI just like a person can. Run the build tree.

### 1. Build it and run it where it is

A Release build now runs from `build/Release` as-is: a `windeployqt` step stages
the Qt runtime beside the exe and the libmpv/SDL2 DLLs are copied in alongside
it (`EB_WINDEPLOYQT`, on by default on Windows — pass `-DEB_WINDEPLOYQT=OFF` if
you are relinking in a loop and don't need to run what you built). Before that
existed, `build/Release/EverythingBox.exe` died instantly with `0xC0000135`
(STATUS_DLL_NOT_FOUND) even with Qt's `bin/` on `PATH`, because libmpv is a
load-time dependency that lives somewhere else again — and the workaround was to
copy an entire real installation and drop the new exe into it, which is slow,
and runs new code against real data.

### 2. Seed the data directory, or you will be driving the first-run wizard

The desktop build is portable: **its data directory is the folder the exe is in**
(`build/Release`), so a fresh build starts factory-new. Two things then stand
between you and the screen you wanted, and both eat your keystrokes:

* the profile picker (and, on a fresh install, the onboarding choice);
* the FORCED theme pick — a profile with no stored theme gets "Pick your look"
  pre-home, and it deliberately has no exit other than choosing.

Stage the stock themes and seed those answers first:

```bash
cp -r native/themes2 build/Release/themes2
```

```ini
; build/Release/everythingbox.ini
[profiles]
list="[{\"icon\":\"\",\"id\":\"test\",\"name\":\"Test\",\"restricted\":false}]"
current=test
skipPickerWhenSingle=true

[onboarding]
done=true

[themedHome]
theme\test=Triple
```

`theme\test` is `themedHome/theme/<profile id>` — it is **per profile**, so it
has to name the same id as `current` or the forced pick fires anyway. Without
`themes2/` the themed home has nothing to render and falls back to the classic
one with a notice, which is not the surface you meant to test.

### 3. Launch with the channel on, and read stderr

```powershell
$env:EB_UITEST = "1"
$env:EB_UITEST_PIPE = "EB-mytest"        # your own channel name; see below
Start-Process build\Release\EverythingBox.exe -WorkingDirectory build\Release `
  -RedirectStandardError eb-stderr.txt
```

`EB_UITEST=1` (or a `--uitest` argument, or the Settings ▸ Debug toggle) turns
the channel on. `EB_UITEST_PIPE` gives this instance its own channel name and
you should always set it: the default name is `EverythingBox-uitest`, and if any
other EverythingBox is already serving it, **this one refuses the name and says
so on stderr** rather than standing a second server on it — because Windows will
happily let both listen and then route each client to whichever it feels like,
which means a harness "passing" against the wrong app.

Redirect stderr. The app is a GUI-subsystem binary with no console of its own,
so that file is where you see the channel refuse to come up. (The same lines
also land in `build/Release/stream_debug.log`, which is where to look if you
forgot to redirect.)

### 4. Drive it

```bash
EB_UITEST_PIPE=EB-mytest python native/tools/uitest.py status
EB_UITEST_PIPE=EB-mytest python native/tools/uitest.py state
EB_UITEST_PIPE=EB-mytest python native/tools/uitest.py keys "right enter down enter"
EB_UITEST_PIPE=EB-mytest python native/tools/uitest.py shot C:/tmp/after.png
```

`state` is a JSON snapshot (page, focus, overlay selection, themed selection,
panel row, reader page…); `shot` renders the window even while it is occluded or
backgrounded; `touch` synthesizes real touch. `uitest.py` with no arguments
lists the rest.

`status` answers `ok ready` once the main window exists and `ok starting` before
that. The channel starts listening in `main()`, **before** the asset bootstrap,
the brand migration, the cloud pull and the whole `MainWindow` constructor, so a
startup that never reaches the window is diagnosable instead of silent: the
connect succeeds, and you get `ok starting` (or, if the app is wedged in
straight-line startup code rather than an event loop, a connect that succeeds
and a reply that never comes). Before issue #172 the server was created ~400
lines into the `MainWindow` ctor, so anything that stalled on the way there left
**no pipe at all** — which looks exactly like an app that was never launched
with `EB_UITEST`, and reads to a harness like nothing being wrong.

### Notes

* Driving the app writes into `build/Release` (its ini, `stream_debug.log`,
  `.assets-version`, `metadata/`…). That is fine for the probe suite — the
  `exe-folder contamination` gate compares that folder before and after its own
  run — but **do not leave the app running while the suite runs**, or the gate
  will (correctly) report that something changed the folder underneath it.
* A Home/recent item takes two Enters: the first opens its detail overlay, the
  second launches.
* If `state` starts taking seconds to answer, that is not the harness. The
  channel is served on the GUI thread, so a slow round-trip *is* a blocked GUI
  thread — which is a finding, not an obstacle.

## Rules that the review will hold you to

### All modal UI goes through `src/ui/nav`

`native/src/ui/nav` is the navigation kit: `NavRing` turns a container's
focusable widgets into one geometric selection ring, `NavContext` routes every
controller/arrow key and guarantees a selection can never be lost, and
`NavOverlay` (with `NavMenu`, `NavConfirm` and `Osk`) draws menus, confirmations,
prompts and the on-screen keyboard as **children of the main window**.

Do not add `QDialog`, `QMessageBox`, `QInputDialog`, or any top-level window to
a navigable surface. A separate OS window black-flickers over the QML themed
surface, fights the desktop for focus, and is unreachable with a D-pad — which
is the whole point of the kit. The overlay system exists precisely to replace
those three classes; use it.

`probe_nav` asserts the invariants (a selection always exists, arrows clamp and
recover from deleted rows, overlays stack and unwind restoring focus, Back
always routes, the OSK works) and gates CI.

**A QML scene is never a ring stop.** `NavRing` refuses `QQuickWidget`s whatever
focus policy they carry. That is not a special case bolted on — it is how the app
already routes QML: a themed page owns its own focus, `MainWindow` hands it
`setActiveRing(nullptr)`, and its selection surface is registered as a `NavGraph`
instead. The refusal exists so that a theme render *embedded in someone else's
widget surface* — a preview — cannot silently become a stop with no action and no
focus outline, which reads to the user as the D-pad selector vanishing (issue
\#40, closed ring-side by \#173 and constructor-side by `ThemeEngine::buildPreview`,
\#123). If you need a QML surface the ring can land on, it is a `NavGraph`, not a
ring member. `probe_navqml` §23 pins both halves.

### `openGeneralSettings()` has two builders — add to both

`MainWindow::openGeneralSettings()` in `native/src/ui/MainWindow.cpp` is written
twice. The first half (under `themedHomeEnabled() && themedPanelHost_`) builds
the setting as a `PanelRow` descriptor list on the themed panel host. The second
half builds the same setting as a classic `QWidget` form.

The themed surface is the **default-reachable** one. A user-facing setting added
only to the classic builder is a setting most users cannot find. Both halves
write the same `Settings` key through the same setter, so adding to both is
mechanical — but it is not optional, and forgetting it is the single most common
way a new option ships invisible. The same doubling applies to the other panels
`MainWindow` builds; if you are editing a settings surface, check whether it has
a themed twin before you assume it doesn't.

This rule is now **enforced**, by the `general settings builder parity` gate in
`native/tools/run-headless-probes.sh`. It reads the themed builder's row ids —
the ids *are* the setting list — and requires every control row (toggle, action,
text field, choice) to name the classic construction that twins it, in both
directions. Add a themed row and the gate tells you what the classic builder
still owes; add a classic control and it tells you the same in reverse. If a row
genuinely belongs to one surface only, say so in `GS_THEMED_ONLY` /
`GS_CLASSIC_ONLY` **with its reason** — an unexplained exemption is
indistinguishable from an oversight, and a stale one fails the gate. Separator
and Info rows are out of scope: they are headings and status read-outs, not
capabilities. The gate covers `openGeneralSettings()` only; the other doubled
panels are still on you.

The theme gallery is the worked example, and the reason there is now a gate:
`RegistryBrowser` carried a `Themes` kind for a long time while its only
construction passed `Addons`, so there was no in-app theme gallery on either
surface and nothing said so. `=== appearance theme-gallery reachability ===`
asserts that every call site the two builders need is still in the source — the
themed one's row, the dispatch arm that handles it, and `presentThemeRegistry`
defined *and* called; the classic one's `RegistryBrowser::Themes` construction.

It reads comment-stripped text, so it cannot see `#if 0` or a runtime `if` — and
it only reads `native/src/ui/MainWindow.cpp`. Move either builder into another
file and the gate goes green while asserting nothing, which is why its first
check is that `MainWindow::openAppearance` is still defined in the file it just
read. The script's own comment says the same; keep the two in step.

### A local leaf kind is a row in one table, not a branch in two functions

A leaf's Enter reaches playback down two paths. The classic grid calls
`HomeView::activateItem`; the themed (Triple/XMB) column opens an inline
Play / Favorite / Add-to-playlist chooser, and the chooser's Play calls
`HomeView::playThemedLeaf`. The themed one is the layout most people run.

Both have to answer the same question about a row — "is this a file this
machine already has, which no addon can resolve?" — and for a long time both
answered it from a list of mimes and types written out by hand, in two places.
The two lists drifted three ways before anyone noticed, because each drift is
invisible from the layout you are testing on: the row plays perfectly in the
classic grid and answers `Nothing to play` on the themed XMB. It shipped that
way for a music track (\#74), a photo (\#102) and an OPDS book (\#146).

So there is no list. `native/src/browse/LeafRoute.h` holds the kinds table, and
both functions dispatch through `browse::localLeafRoute`. **Adding a local kind
is one row in `localLeafKinds()`** — declare its spelling in the marked
`LOCAL LEAF KINDS` block, have the catalog builder stamp rows from that constant,
and add the table entry. A new *route* (a kind that means something other than
"open this url") additionally needs an arm in both switches.

`=== themed local-leaf routing parity ===` in the suite enforces exactly that:
both functions must call `localLeafRoute`, every `LeafPlay` enumerator must be
handled in both, and every constant declared in the marked block must appear in
the table. A route that genuinely belongs to one surface goes in
`LR_SURFACE_ONLY` **with its reason**, and a stale exemption fails — same
discipline as the settings-builder gate above. `probe_leafroute` pins the
decisions themselves, driving the real catalog builders end to end so the claim
is "a local leaf activated through the themed path reaches a player" rather than
"the table agrees with itself".

The corollary is the reason the gate exists at all: **verify on the themed
layout.** Every fault this rule is written from was green under `probe_nav` and
`probe_themeview`, worked in the classic grid, and was found by a person driving
the app by hand. When you touch browse, routing or a home category, drive it
through the uitest channel on Triple before you call it done — the Photos
category had no themed home at all for as long as it has existed, and the
classic grid showed it the whole time.

### A new pure component gets a probe, registered in three places

Extract logic into something testable without a window, then add a probe under
`native/tools/`. It prints a sentinel like `FOO-OK` on success and returns 0.
Register it in **all three** of:

1. its `add_executable(probe_foo …)` + `target_link_libraries` in
   `native/CMakeLists.txt`;
2. the runner list in `native/tools/run-headless-probes.sh` — for a plain
   no-argument probe that is the `for p in "probe_… …-OK"` loop near the end;
   probes needing arguments or a platform flag get their own `run` line;
3. the `--target` list in the "Build probes" step of
   `.github/workflows/ci.yml`.

Miss any one of them and the probe silently never runs. That is not
hypothetical: `probe_addon` was written and maintained for a long time while
being wired into neither the runner nor CI, so every assertion in it gated
nothing. Adding a probe target is not the same as running it.

### An assertion is proven by mutation — and there is one driver for it

A passing probe proves nothing on its own. For each assertion you add, break the
behaviour it guards and show the assertion goes red. An assertion no mutation
kills is either inert — fix it or delete it — or a deliberate
absence-of-behaviour tripwire, in which case say so in a comment where it lives.
A guard that cannot fire is worse than none, because it reads as protection.

**Do not write your own driver for that loop.** `native/tools/mutate.py` is it.
Every hand-rolled copy has rediscovered the same trap, and it is not a trap you
can be careful about:

```bash
native/tools/mutate.py --spec my-matrix.json
native/tools/mutate.py --selftest      # what the suite's `mutation driver rule` gate runs
```

```jsonc
{
  "build":    ["cmake", "--build", "build", "--config", "Release", "--target", "probe_marks", "--parallel"],
  "test":     ["build/Release/probe_marks.exe"],
  "artifact": "build/Release/probe_marks.exe",   // its mtime must advance, or the build did nothing
  "sentinel": "MARKS-OK",                        // exit 0 without this is not a pass
  "env":      { "QT_QPA_PLATFORM": "offscreen" },
  "mutants": [
    { "name": "husk-never-known-guard",
      "file": "native/src/core/ItemMarks.cpp",
      "find": "…the two lines as they appear in the file, \n is fine…",
      "replace": "…one of them…",
      "count": 1,                                // occurrences; a mismatch stops the run
      "expect": "killed" }                       // optional; a different verdict fails the run
  ]
}
```

It reports **three** outcomes, and the third is the reason the file exists:

| Outcome | Means |
|---|---|
| `KILLED` | applied, rebuilt, the test went red — the assertion discriminates |
| `SURVIVED` | applied, rebuilt, the test stayed green — audit that assertion |
| `NOT APPLIED` | **no verdict was reached.** A run containing any of these is a failed run, not a result (exit status 2) |

An unapplied mutation is indistinguishable from a surviving one from outside:
the test passes, because the code under it never changed. A driver that collapses
the two reports `SURVIVED`, which reads as "this assertion is inert" — and that
is the verdict that gets a *working* assertion deleted. Three independent agents
hit exactly that on one day (issues #123, #151, #164) and it is the whole of
issue #175.

The specific cause, every time: **this working tree is CRLF**
(`core.autocrlf=true`, no `.gitattributes`), and a multi-line pattern written
with `\n` — which is what a Python string, a heredoc, or anything typed on a
Unix-shaped keyboard gives you — matches nothing at all. `mutate.py` compiles
every line break in an anchor to `(?:\r\n|\n)`, so both spellings apply, and
re-encodes the replacement to the line endings the file actually uses so nothing
outside the mutated span is rewritten. It also:

* **verifies the edit landed** — re-reads the file and compares bytes — before
  building anything. `git diff` is a cross-check, not the authority: with
  `autocrlf` on, git compares *normalised* content, so it cannot see a change
  that is purely line endings;
* **restores the source with a refreshed timestamp.** Restoring a backup with
  `mv` (or `copy2`, or `cp -p`) carries the backup's *old* mtime back. MSBuild
  then decides the object is newer than the source, skips the compile, and the
  reverted tree goes on testing as mutated — measured, not theorised: a `mv`
  restore plus a full rebuild still failed `probe_marks` on the mutant's
  assertion, and a `touch` plus the same rebuild passed;
* **stops on a drifted anchor.** Zero matches is fatal, and so is more matches
  than you declared — an ambiguous anchor would mutate a site you did not choose;
* refuses to call a **build failure** a kill (a mutant that does not compile
  says nothing), refuses a verdict when the declared `artifact` did not rebuild,
  and refuses to call a test that exited 0 without printing its sentinel a
  survivor;
* rebuilds once from the restored source at the end, so the tree's binaries and
  its source agree again. Pass `--no-final-build` only if you are about to
  rebuild anyway.

The `=== mutation driver rule ===` gate in the suite runs `--selftest`, which
drives real matrices against a throwaway CRLF subject and requires each of those
outcomes to still be told apart. Add a behaviour to the driver, add its case.

### A probe's data directory is its own — you get that for free

`AppPaths::dataDir()` is where the app keeps `everythingbox.ini`, `addons/`,
`metadata/`, `saves/` and the rest. On desktop the app is portable, so that is
the executable's own folder — which is also `build/Release`, where every probe
binary is built next to the GUI exe. One ini, shared between the app, the
probes, and anything a developer dropped in that folder.

Every target named `probe_*` is therefore compiled with `EB_ISOLATED_DATA_DIR`,
which points `dataDir()` at a scratch directory created **per process** and
removed when the process exits. You do not opt in and there is nothing to
remember: the define is applied by name over every probe target at the bottom of
`native/CMakeLists.txt`. Two consequences worth knowing:

* a probe's `dataDir()` is empty when it starts and gone when it ends, so a
  probe does not defensively wipe the groups it uses and does not clean up after
  itself for the next run — see *Do not write a defensive reset* below — and a
  probe that wants a fixture on disk must write it under `dataDir()`, not next
  to the exe;
* nothing in `build/Release` can change a probe's result, and no probe can
  change what is in `build/Release`. `probe_isolation` asserts both (the runner
  seeds junk into that folder before running it), and the suite's
  `=== exe-folder contamination ===` gate compares a *fingerprint* of the folder
  before and after the run. Know what that fingerprint is, because the next
  person debugging a suite failure will lean on it: the top-level entry names,
  the recursive file list of the app-data subdirectories (`addons/`,
  `metadata/`, `themes/`, `saves/`, …), and a checksum of `everythingbox.ini`,
  its pre-rebrand counterpart, and `saves-meta.json`. So anything appearing,
  vanishing or being renamed is caught, and so is any edit to those three
  files — but an **in-place edit that leaves the file list unchanged** is
  not. Rewriting `addons/<x>/main.js`, or a file under `themes/`, passes. It is
  a heuristic aimed at the shape the real collisions took, not a byte-for-byte
  comparison of the folder.

`EB_PROBE_DATA_DIR_KEEP=1` keeps the scratch directory around when you need to
see what a failing probe wrote; `EB_PROBE_DATA_DIR=<dir>` pins it.

### Do not write a defensive reset (#48)

A dozen probes used to open the shared ini at startup only to `remove()` the
group they were about to use, or to delete their own scratch files on the way
out so the *next* run would start clean. Issue #48 swept all of it. Do not add
another: there is no previous run in that directory and no sibling probe in it,
so the reset defends against nothing and reads to the next author as the house
pattern.

The line to hold is **what the reset is defending against**:

* against a *previous run*, a *sibling probe*, or anything a developer dropped
  in `build/Release` → delete it. Isolation covers that case completely.
* against an *earlier section of the same probe*, in the same process → keep it,
  and say so in the comment. Isolation is per process and says nothing about
  what case 3 left for case 4. `probe_savesync`'s `clearTombs()`,
  `probe_cloudmerge`'s `wipeStores()`, `probe_brand`'s per-section
  `clearAllFlags()` and `probe_navqml` §20's mode restore are all this, and all
  stayed.
* a fixture the probe then asserts on → keep it. Seeding is not resetting.
  `probe_playlists` still writes the v1 blob it migrates.

Deleting a reset is not a tidy-up, so do not treat it as one. A probe that
passed only because it controlled the state has assertions that were partly
inert, and the removal is what exposes them. Mutate the code the probe is about
and check the probe actually goes red; if it does not, the assertion is the bug.
That is how #48 found that nothing in the suite pinned `Settings::externalPlayer()`'s
unconfigured default — `probe_extplayer` set the key before every check it made,
so flipping the default from `builtin` to `vlc` left the suite green. Fixing
that needed a store nobody had written to, which is exactly what isolation now
provides. Strengthen the assertion; never restore the reset to hide the gap.

This does **not** replace the per-unit `setIniPathForTesting` seams
(`ThemeChoice`, `SettingsTxn`, `ProfilePasscode`, `PcGameId`, each behind its own
`EB_*_TEST_SEAM`). They answer a different question. Isolation is per *process*,
and every core unit caches its `QSettings` in a function-local static on first
use — so one process gets exactly one store. A probe that needs several
independent stores, or needs to re-open one after poking its file from outside,
still needs the seam; `probe_theme` drives six separate scratch inis through one
process for precisely that reason. What isolation *does* remove is the seams'
other motive — "don't write the real ini" — so a new probe should not grow a new
seam for that alone.

### A theme that ships here also ships in the registry — keep the record in step

`native/themes2/Channels`, `Night` and `Triple` exist twice: bundled here, and
published in the community registry
([everythingbox-themes](https://github.com/cubman3134/everythingbox-themes))
that the Appearance panel points users at. So does `themes2/THEME_FORMAT.md`.
Both copies had drifted badly before anything noticed (issue #57) — the
registry's Triple had lost every view but `home`, which is exactly the blank
cross-add-on search of issue #29, and downloading a theme from the registry got
you a strictly worse one than the app already had under the same name.

`=== bundled-theme / registry drift ===` fails the moment a bundled theme's
*meaning* changes (the hash is of the parsed, re-serialised JSON, so a reformat
is free). When it goes red, run

```
native/tools/theme-registry-sync.py --update --assume-published "published on merge"
```

and commit the refreshed `native/themes2/REGISTRY-SYNC.json` with the theme
change. That file documents the procedure, and is also where a theme gets
recorded as deliberately *not* published.

**A bare `--update` is refused** (issue #151). It recomputes the record from the
*bundled* theme and never looks at the registry, so on its own it records
"somebody ran this" — and that is how the record came to assert a publish that
had not happened, with the registry serving the pre-#29 Triple under a green
gate for days. The record must name either the registry commit it was published
as (`--registry-commit <sha>`) or a written reason it has none
(`--assume-published "<why>"`). The gate prints whichever it is on every run.

In a branch that edits a theme, `--assume-published` is the *correct* answer,
not a dodge: the copy has genuinely not happened yet. **You do not copy anything
into the registry yourself.** On merge to `main`, the `publish themes` workflow
checks the registry out with a deploy key, copies over exactly the targets that
record lists, pushes, then reruns
`--update --registry-commit <the sha it just created>`, commits *that* back to
this repo, and finally re-fetches what the registry serves to confirm it
matches. So a `registryCommit` in the record is normally machine-written and
substantiated by construction. Use `--registry-commit` by hand only if you
published by hand.

`verify registry` re-runs the same check every Monday, which catches what the
publisher structurally cannot see: a direct edit there, a revert, or a publish
that failed and was never retried. To test the claim yourself, from anywhere
with a network call:

```
native/tools/theme-registry-sync.py --verify-registry
```

It exits `0` for a match, `1` for real drift and `2` for "could not find out" —
an unreachable registry is not a verdict about the record. Never run it from the
probe suite: that suite is offline by design, so the record is what goes red in
your PR and the workflows are what make it true afterwards.

Adding a *new* theme to the registry is still a two-repo change. `--publish`
refuses to *create* a path the registry does not already carry, because
`index.json` needs a `description` that exists nowhere in `theme.json` and a
folder nothing lists is a theme nobody can find. Do the registry side first.

### The registry serves four themes this repo has never seen

`Default`, `Grid`, `Lumen` and `Midnight` exist only in the registry, so the
drift gate above cannot check them at all. `native/tools/theme-registry-validate.py`
is the rule that can: `index.json` and each `theme.json` must agree on `name`,
`author` and `formFactors`, every theme must parse and declare a view with
elements, and no published folder may be missing from the index. It lives here
because this repo defines what those fields mean, and the registry's CI
downloads and runs it — the same arrangement its `theme-assets.yml` already uses
for the app's `Theme.js`.

`=== registry index / manifest rule ===` runs `--selftest`, which proves each of
those checks fires on the defect it names. Add a check, add its mutation.

If the check you add is a *could-not-run* branch — "this is not a registry
checkout", "index.json does not parse" — add it to `FATAL_CASES`, not
`MUTATIONS`, and note that those cases assert the process **exit status**. A
permissive edit to a per-theme check lets one bad submission through; a
permissive edit to a could-not-run branch makes the file pass on anything it is
pointed at, and the registry's CI runs `--selftest` on the copy it downloaded
precisely so that file cannot then bless a PR.

### The old brand stays gone

The product was renamed. The probe suite enforces that as a property rather
than a claim: `=== old-brand references ===` greps the whole tree for the
previous name and fails on any hit outside a list of documented exemptions at
the top of that section in `run-headless-probes.sh`.

If your change trips it, the fix is almost always to use the current name. If
you genuinely need an exemption — a migration path, a fixture of what old
installs wrote, a deployed identifier that cannot be renamed without orphaning
users — add it to that list **with its reason**. An unexplained exemption is
indistinguishable from an oversight.

## Commits

Conventional prefixes:

- `feat:` — new user-visible capability
- `fix:` — a bug fix
- `docs:` — documentation only
- `refactor:` — behaviour-preserving restructuring

Write the body for whoever reads it in a year. This codebase's comments explain
*why* a thing is the way it is, often at length; commit messages are held to the
same standard. If you worked around something surprising, say what surprised
you.

## Reporting bugs and proposing features

Use the issue templates in `.github/ISSUE_TEMPLATE/`. For a bug, the log at
`stream_debug.log` in the app data directory is usually the fastest route to a
diagnosis — Settings ▸ Debug shows its tail in-app and can open its folder.

For design discussion *before* you write code — a protocol change, anything
touching the nav kit, a settings surface you suspect has a themed twin —
`#dev-general` on the [Discord](https://discord.gg/bW7KMVhgwH) is far lower
latency than issue comments, and it is much cheaper to learn there that an
approach is wrong than to learn it in review. Add-on and theme authors have
`#addon-development` and `#theme-development`. Whatever is decided in chat still
gets written down in the issue or the pull request: chat is where a decision is
reached, not where it is recorded.

By participating you agree to the [Code of Conduct](CODE_OF_CONDUCT.md).
Contributions are accepted under the [GNU General Public License v3.0](LICENSE).
