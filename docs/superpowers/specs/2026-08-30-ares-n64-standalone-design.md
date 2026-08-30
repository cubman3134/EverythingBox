# ares as the default N64 emulator

Date: 2026-08-30
Branch: `feat/ares-n64`
Worktree: `C:/Users/cubma/goliath-wt-ares`

## Goal

Add [ares](https://ares-emu.net/) to EverythingBox as a standalone (external-process)
emulator and make it the default engine for the `n64` system, the way `psx` already
defaults to standalone DuckStation.

Three things have to come with it, because the one-field change alone would regress
two shipped behaviours and leave the emulator unplayable on first launch:

1. a platform gate, so a standalone default does not kill N64 on Android TV / iOS;
2. libretro cores staying reachable in the picker for a standalone system;
3. an input seed, because ares ships with **no** input bindings at all.

## What was verified about ares (v148, read from source — not from memory)

These are the facts the design rests on. Each was read out of the ares tree at
`ares-emulator/ares@master`, or out of the GitHub releases API, on 2026-08-30.

* **Command line** is `ares [options]... game(s)` (`desktop-ui/desktop-ui.cpp`).
  Relevant flags: `--fullscreen`, `--no-file-prompt`, `--setting name=value`,
  `--settings-file path`, `--system name`.
* **The system is auto-detected** from the ROM (`identify(gameToLoad)` in
  `desktop-ui/program/program.cpp`), so `--system` is unnecessary — which matters,
  because `argsTemplate` is split on spaces and could not quote `"Nintendo 64"`.
* **`--no-file-prompt` is mandatory.** `Nintendo64::load()` opens a blocking file
  dialog for any cart carrying the `dd` (64DD) or `tpak` (Transfer Pak) attribute
  unless `settings.general.noFilePrompt` is set.
* **No BIOS is needed.** `mia/system/nintendo-64.cpp` appends `pif.ntsc.rom`,
  `pif.pal.rom` and `pif.sm5.rom` from `Resource::Nintendo64::…` — they are compiled
  into the binary. So no `BiosCatalog` entry, and `systemNeedsBios("n64")` stays false.
* **Windows is portable by default.** `locate()` checks the program directory first
  and, on Windows, falls back to the program directory when the file is absent
  anywhere — so `settings.bml` lands beside `ares.exe` in our `emulators/ares/`.
  No portable marker to write.
* **Release assets** (v148): `ares-windows-x64.zip`, `ares-windows-clang-cl-arm64.zip`,
  `ares-macos-universal.zip`, plus `-PDBs.zip` / `-dSYMs.zip` debug archives and a
  source tarball. **There is no Linux binary** — Linux is Flathub `dev.ares.ares`.
* **ares has no default input bindings.** There is no auto-map, no keyboard default,
  and no first-run assignment anywhere in `desktop-ui/input/` or
  `desktop-ui/settings/input.cpp`. A fresh install boots a game you cannot control.

### The input format, in detail

`desktop-ui/settings/settings.cpp` binds each virtual-pad input to the settings path
`VirtualPad{1..5}/{name}`, where `name` is the input's display name pushed through
`.replace(" ", ".").replace("(", ".").replace(")", "")`. From
`VirtualPad::VirtualPad()` in `desktop-ui/input/input.cpp`, the names we care about
therefore serialize as:

| ares display name | settings key |
|---|---|
| `Pad Up` / `Pad Down` / `Pad Left` / `Pad Right` | `Pad.Up` / `Pad.Down` / `Pad.Left` / `Pad.Right` |
| `A (South)` / `B (East)` / `X (West)` / `Y (North)` | `A..South` / `B..East` / `X..West` / `Y..North` |
| `Select` / `Start` | `Select` / `Start` |
| `L-Bumper` / `R-Bumper` / `L-Trigger` / `R-Trigger` | unchanged |
| `L-Stick (Click)` / `R-Stick (Click)` | `L-Stick..Click` / `R-Stick..Click` |
| `L-Up` / `L-Down` / `L-Left` / `L-Right` | unchanged |
| `R-Up` / `R-Down` / `R-Left` / `R-Right` | unchanged |
| `Rumble` | `Rumble` |

The N64 core reads that abstract pad, not the raw device:
`desktop-ui/emulator/nintendo-64.cpp` wires N64 `A` to `pad.south`, `B` to `pad.west`,
the C-buttons to the **right stick** directions, `Z` to `pad.r_trigger`, `L`/`R` to the
bumpers. So seeding the virtual pad is enough; nothing N64-specific needs writing.

An assignment value is parsed by `InputMapping::bind()` in `desktop-ui/input/input.cpp`
as slash-separated tokens. Two shapes exist; the joypad one is

```
<identity>/<slot>/<groupID>/<inputID>[/<qualifier>]
```

where `identity` is the **SDL GUID string** and `slot` disambiguates two identical
pads (`ruby/input/joypad/sdl.cpp` sets `identifier = {identity, "/", slot}`). The
group IDs come from `nall/nall/hid.hpp`:

```
HID::Joypad::GroupID { Axis = 0, Hat = 1, Trigger = 2, Button = 3 }
```

`inputID` is the **raw joystick** index, because ares' SDL driver enumerates
`SDL_GetNumJoystickAxes` / `…Hats` / `…Buttons` rather than the gamepad abstraction.
Each SDL hat is split into two ares hat inputs (X at `hat*2`, Y at `hat*2+1`), selected
by a trailing `Lo` / `Hi` qualifier. Analog half-axes use the same `Lo` / `Hi`.

`assignments[]` holds up to `BindingLimit` entries per input, so more than one binding
per control is legal.

## Design

### 1. The registry entry

Append one `ExternalEmulator` to `EmulatorRegistry::builtinEmulators()`:

| field | value |
|---|---|
| `id` | `ares` |
| `displayName` | `ares` |
| `argsTemplate` | `{fs} --no-file-prompt {rom}` |
| `fullscreenArgs` | `--fullscreen` |
| `windowedArgs` | *(empty)* |
| `homepage` | `https://ares-emu.net/` |
| `winBinaries` | `ares.exe`, `ares/ares.exe` |
| `macBinaries` | `ares.app/Contents/MacOS/ares`, `ares.app` |
| `linuxBinaries` | `ares` |
| `updateJsonUrl` | `https://api.github.com/repos/ares-emulator/ares/releases/latest` |
| `winArtifact` | `ares-windows-x64.zip` |
| `macArtifact` | `ares-macos-universal.zip` |
| `linuxArtifact` | *(empty — Flatpak)* |
| `flatpakAppId` | `dev.ares.ares` |
| `extensions` | `n64`, `z64`, `v64`, `ndd` |
| `systems` | `n64` |

The artifact markers are **full filenames including the extension**, not the usual
short platform marker. `assetMatches` currently rejects a name containing `pdb`
(so `ares-windows-x64-PDBs.zip` is already safe) but **not** one containing `dsym`,
and `ares-macos-universal-dSYMs.zip` is listed *before* the real archive in the
assets array. A short `macos-universal` marker would download the debug symbols.

As defence in depth, `"dsym"` is added to the exclusion list in `assetMatches`
([`native/src/core/EmulatorManager.cpp`](../../../native/src/core/EmulatorManager.cpp)).
No shipped emulator wants an artifact whose name contains `dsym`, so this can only
ever exclude a debug-symbol archive.

`resolveBinary`'s recursive-by-filename fallback already handles an archive that
extracts into a versioned subfolder, so the two `winBinaries` candidates suffice
whichever way `ares-windows-x64.zip` is laid out.

### 2. Wiring `n64` to it

In `SystemCatalog::builtinSystems()`, the `n64` row gains `"ares"` as its
`externalEmulator`, keeping `mupen64plus_next` / `parallel_n64` in `cores` — the same
shape `psx` already has, and for the same stated reason (removing the one field
restores the in-process path).

No `BiosCatalog` change: `forExternalEmulator("ares")` keeps returning the empty
default, and `n64` is correctly absent from `kBiosSystems`.

### 3. The `standaloneAvailable` gate

`GameLauncher::open` refuses every standalone system on Android and iOS — the
sandbox cannot spawn a downloaded desktop executable. Making `n64` standalone would
therefore kill N64 on the onn box, where `mupen64plus_next` works today.

Add a third platform bool to the pure model in
[`native/src/core/EmulationTarget.h`](../../../native/src/core/EmulationTarget.h),
mirroring the existing `retroParkAvailable` exactly:

* `emulationTargetsFor(sys, retroParkAvailable, standaloneAvailable)` — when
  `standaloneAvailable` is false, no standalone target is offered.
* `resolveEmulationTarget(…, standaloneAvailable)` — same, for the current value.
* `resolveLaunch(…, standaloneAvailable)` — when false and the resolved engine is
  `Standalone`, degrade to `Libretro` **if and only if** `sys->cores` is non-empty,
  resolving the core through the existing `LaunchOpts::resolveCore` path.

The `cores`-non-empty condition is what keeps the change safe: `gc`, `3ds` and `nds`
declare no cores, so they do not degrade and keep today's "isn't supported on
Android" message byte-for-byte. `n64` keeps mupen64plus_next there. `psx` — which has
`swanstation` and is currently dead on Android TV despite it — starts working.

The value is supplied the same way `kRetroParkBuildAvailable` is: a
`constexpr bool` that is false under `Q_OS_ANDROID` / `Q_OS_IOS` and true elsewhere,
defined once beside the launcher's existing gates and once beside `MainWindow`'s, so
the picker never offers a target `prepareCore` would degrade away.

### 4. Cores in a standalone system's picker

`emulationTargetsFor` today emits, for a standalone system, only its bound emulators
(then RetroPark). Its libretro cores become unreachable from both the per-game
Emulation picker and the per-system Settings list — both of which call this one
function.

Add a third block so the order for a standalone system becomes:

1. the system's own `externalEmulator`, then any registry emulator bound to the
   system via `e.systems` (unchanged);
2. **then one libretro target per entry in `sys->cores`** (new);
3. then the RetroPark target, where supported and available (unchanged).

A libretro system is untouched (it already lists its cores first). This is what gives
a user on a weak HTPC a route back to `mupen64plus_next` for one game or for all of
N64, and it is coherent with §3, which already names those cores as the platform
fallback.

### 5. Seeding ares' input

**New pure header** `native/src/core/AresInput.h` — QtCore-only, no SDL, no disk, in
the style of `ControllerSeats.h` / `EmuSettings.h`. It exposes one function taking the
assigned seats and returning the `settings.bml` body to seed.

**`ControllerSeats::PadInfo` gains an `sdlMapping` field** holding that pad's SDL
gamepad mapping string. `enumerateConnectedPads` in `EmulatorManager.cpp` fills it
from `SDL_GameControllerMappingForDeviceIndex(i)`; it already fills `guid` and `name`
from the same loop, and already loads `gamecontrollerdb.txt` so the mapping matches
the in-process tier's. Adding the field does not disturb `PadInfo::operator==`, which
compares `index` / `guid` / `name`.

The translation, per seat *n*:

* parse the pad's SDL mapping string into logical-control → raw-input bindings
  (`a:b0` → button 0, `leftx:a0` → axis 0, `dpup:h0.1` → hat 0 bit 1, with SDL's
  `+`/`-`/`~` half-axis and inversion prefixes honoured);
* emit `VirtualPad{n+1}/{key}` for each key in the table above, with the assignment
  `<pad.guid>/0/<groupID>/<inputID>[/<qualifier>]`;
* a control the pad's mapping does not declare simply gets **no** assignment — the
  same "degrade, never guess" posture `ControllerSeats::controllerEdits` takes for an
  unknown emulator.

The `slot` term is `0` for every seat: seats are distinguished by their *VirtualPad
number*, not by the slot, and slot is ares' tiebreaker between two pads reporting the
same GUID. Two identical pads therefore both resolve to slot 0 in our seed; the second
one is a known limitation, recorded below.

**Write side.** `EmulatorManager::prepareControllerConfig` gains an `ares` arm. Unlike
the other emulators it is *not* per-seat — `settings.bml` is one file covering all
four virtual pads, so the arm calls `AresInput` once with the whole seat list and
writes `settings.bml` **only when the file contains no `VirtualPad` assignment at
all**. That covers both the fresh install and the case where ares had been launched
once (and so wrote a full but unmapped file) before we ever seeded. A file with any
existing assignment — a user's own mapping — is left untouched.

### Rejected alternatives

* **Keyboard seed + a built-in `n64` pad2key profile.** Deterministic (ares' keyboard
  device id is a constant, needing no GUID match) and it reuses a shipped subsystem,
  but pad2key is digital-only: the N64 analog stick would become on/off, so Mario 64
  could only ever walk at full tilt. Rejected on quality.
* **Seed nothing, prompt once.** Zero risk, but "the default N64 emulator" would ship
  unplayable until the user mapped 16 controls in ares' own dialog.
* **`--setting VirtualPad1/…=…` on every launch** instead of writing `settings.bml`.
  Stateless and it can never clobber, but by the same token it would override a user's
  own remap on every single launch. Kept as a debugging aid, not the mechanism.
* **A platform-conditional `externalEmulator`** (`#if !defined(Q_OS_ANDROID)…` inside
  the catalog table) instead of §3. Narrower, but it puts platform ifdefs into a pure
  data table the probes read, and leaves PlayStation broken on Android TV.

## Testing

**`probe_aresinput`** (new) pins the pure translation against hand-authored SDL
mapping strings, with no SDL and no disk:

* an Xbox-layout pad (buttons + `a4`/`a5` triggers);
* a DualSense (different raw button order — proves indices come from the mapping
  string, not an assumed layout);
* a pad whose dpad is a **hat** (`dpup:h0.1`) — proves the `hat*2` + `Lo`/`Hi` split;
* a pad missing controls — proves those keys are omitted rather than guessed;
* two seats — proves `VirtualPad1` / `VirtualPad2` numbering;
* the exact settings-key spellings (`A..South`, `L-Stick..Click`, `Pad.Up`).

**`probe_emutargets`** (existing) extends to cover:

* `standaloneAvailable = false` degrading `psx` → `swanstation` and `n64` →
  `mupen64plus_next`, while `gc` / `3ds` / `nds` (no cores) stay `Standalone`;
* `standaloneAvailable = true` leaving every existing resolution byte-for-byte;
* the new picker order for a standalone system, both bools crossed.

**`probe_syscatalog`** / **`probe_useremulators`** get whatever the added rows
require — the registry round-trip (`fromJson(toJson(e)) == e`) must still hold for the
ares entry, and the built-in-table-equals-`all()`-with-no-data-files property must
still hold.

Registration follows the three-places rule (`native/CMakeLists.txt`,
`native/tools/run-headless-probes.sh`, `.github/workflows/ci.yml`); `AresInput.h` is
header-only so the fourth site (the app's own source list) does not apply.

The gate must end `ALL HEADLESS PROBES PASSED`.

## What cannot be verified headlessly

Both of these need a real machine and a real pad, and are explicitly part of "done":

1. **SDL2 vs SDL3 GUID equality.** EverythingBox links SDL2; ares v148 links SDL3.
   The seeded `identity` must be byte-identical to what ares computes or every binding
   is silently inert. Verify by launching ares once from EverythingBox with a pad
   attached and reading back the `settings.bml` ares writes.
2. **BML serialization shape.** Our seeded file must parse as the same document ares
   would write. Verify by generating a real `settings.bml` from a live ares run and
   diffing the shape of ours against it before the branch is called done.

A third, cheaper check: confirm a 64DD-capable cart boots straight to gameplay,
proving `--no-file-prompt` reaches ares intact through the space-split `argsTemplate`.

## Known limitations (documented, not fixed here)

* Two physically identical pads both seed with `slot` 0, so the second is not
  distinguished. ares' own `slot` tiebreaker would need the same enumeration order we
  cannot observe from outside.
* A pad plugged in *after* the seed has been written gets no mapping; the seed is
  once-per-install by design, since re-seeding would clobber a user's own edits.
* ares hotkeys (save state, fullscreen toggle, quit) are left unmapped. Exit is
  already EverythingBox's job — `EmulatorManager` asks the process to close and force-kills
  it after a grace period.
* `EmuGfx::configEdits` emits nothing for `ares` (its config is BML, not INI), so the
  cross-emulator graphics quartet does not reach it. That is the documented
  degrade-to-nothing behaviour for an unsupported emulator. ares' `--setting` flag is
  the obvious future route; out of scope here.
