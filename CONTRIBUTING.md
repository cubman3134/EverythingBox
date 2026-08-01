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
declares 51 probe harnesses in addition to the app, and the default target
builds all of them. That is many minutes of compiling you almost certainly did
not want. Name what you need.

SDL2 (gamepad input) is optional at configure time — without it the app builds
fine and controller input is simply absent. Release binaries do link it; see
`.github/workflows/release.yml`.

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
registry drift gate below, and the old-brand gate. Those fail on code you wrote
even if every probe binary passes. One
more gate is a property of the run rather than of the source: `exe-folder
contamination` compares the build folder's app-data footprint before and after
the suite, and fails if anything changed it while the suite ran — normally a
probe, occasionally an app or a build running out of that same folder.

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

### A theme that ships here also ships in the registry — change both

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
is free). When it goes red, republish the changed folder to the registry, then
run `native/tools/theme-registry-sync.py --update` and commit the refreshed
`native/themes2/REGISTRY-SYNC.json` with the theme change. That file documents
the procedure, and is also where a theme gets recorded as deliberately *not*
published.

The gate cannot see the registry — it is a different repo, and this suite is
offline by design — so it checks the record, not the remote. It makes drift
loud, not impossible; making it impossible means a publish job with a
cross-repo write credential.

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
