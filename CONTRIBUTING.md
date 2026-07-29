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
declares 43 probe harnesses in addition to the app, and the default target
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
`.srm` path gate, and the old-brand gate below. Those fail on code you wrote
even if every probe binary passes.

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
