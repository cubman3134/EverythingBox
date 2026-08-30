# ares as the default N64 emulator — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship [ares](https://ares-emu.net/) as a standalone emulator in EverythingBox and make it the default engine for the `n64` system, without regressing N64 on Android TV / iOS and without leaving the emulator unplayable on first launch.

**Architecture:** Four independent pieces, landed in dependency order so the user-visible flip happens last. A platform gate in the pure `EmulationTarget` model degrades a standalone system back to its libretro cores where external processes are impossible. The picker gains those cores as selectable targets. A new pure header translates a connected pad's SDL mapping string into ares `settings.bml` bindings. Only then does `n64` gain `externalEmulator = "ares"`.

**Tech Stack:** Qt 6 / C++17, header-only pure models under `native/src/core/`, headless probe harnesses under `native/tools/`, CMake multi-config (Release), SDL2 for pad enumeration.

**Spec:** [`docs/superpowers/specs/2026-08-30-ares-n64-standalone-design.md`](../specs/2026-08-30-ares-n64-standalone-design.md)

**Worktree:** `C:/Users/cubma/goliath-wt-ares` on branch `feat/ares-n64`. Every path below is relative to that worktree. All shell blocks are `bash` (Git Bash), run from the worktree root.

## Global Constraints

- **No AI attribution in commits.** No `Co-Authored-By`, no "Generated with", no tool name. Repo rule, `CLAUDE.md`.
- **Conventional commit prefixes** (`feat:`, `fix:`, `docs:`, `refactor:`) per `CONTRIBUTING.md`.
- **This repo has no `CHANGELOG.md`.** Release notes are generated from commits. Do not create or edit one.
- **Never run a target-less `cmake --build build`.** It builds all 52+ probe harnesses. Always name targets.
- **Never use `sed -i` on a tracked source file in this tree.** Confirmed in Task 1: `sed -i` rewrote all of `MainWindow.cpp` from CRLF to LF, producing a whole-file diff. Use the Edit tool, or a byte-preserving Python script that reads and writes `'rb'`/`'wb'`. This matters most for `native/tools/run-headless-probes.sh` (CRLF) and `native/CMakeLists.txt` (hides a lone CR), but it applies everywhere. After any such edit, check `git diff --stat` shows only the lines you meant to touch.
- **A new probe is registered in three places** or it silently never runs: `add_executable` in `native/CMakeLists.txt`, the runner loop in `native/tools/run-headless-probes.sh`, and the `--target` list in `.github/workflows/ci.yml`.
- **The gate must end `ALL HEADLESS PROBES PASSED`.** Run it as `BUILD_DIR=build bash native/tools/run-headless-probes.sh` and read the verdict from `build/headless-probes.verdict`, never from a pipeline's exit status.
- **Build environment** (once per shell):
  ```bash
  export PATH="/c/Qt/6.8.3/msvc2022_64/bin:/c/mpv-dev:$PATH"
  ```
- **Configure** (once per worktree):
  ```bash
  cmake -S native -B build -DEVERYTHINGBOX_BUILD_APP=ON -DCMAKE_PREFIX_PATH="C:/Qt/6.8.3/msvc2022_64" -DMPV_INCLUDE_DIR="C:/mpv-dev/include" -DMPV_LIBRARY="C:/mpv-dev/libmpv.lib"
  ```
- **Fixtures are hand-computed oracles.** Expected values in probes are written as literals derived from the documented rules — never produced by re-running the function under test.

---

## File Structure

**Created**

| File | Responsibility |
|---|---|
| `native/src/core/AresInput.h` | Pure, header-only. Parses an SDL gamepad mapping string and emits ares `settings.bml` bindings for a seat list. QtCore-only, no SDL, no disk. |
| `native/tools/probe_aresinput.cpp` | Headless oracle for `AresInput.h`. Prints `ARESINPUT-OK`. |

**Modified**

| File | Change |
|---|---|
| `native/src/core/EmulationTarget.h` | `standaloneAvailable` parameter on the three model functions; the `kStandaloneBuildAvailable` platform constant; libretro cores appended to a standalone system's target list. |
| `native/src/core/EmulatorRegistry.h` | The `ares` built-in entry. |
| `native/src/core/SystemCatalog.h` | `n64` gains `externalEmulator = "ares"`. |
| `native/src/core/ControllerSeats.h` | `PadInfo` gains `sdlMapping`. |
| `native/src/core/EmulatorManager.cpp` | `dsym` exclusion in `assetMatches`; `sdlMapping` filled in `enumerateConnectedPads`; the `ares` arm of `prepareControllerConfig`. |
| `native/src/launch/GameLauncher.cpp` | Passes `kStandaloneBuildAvailable` to `resolveLaunch`. |
| `native/src/ui/MainWindow.cpp` | Passes `kStandaloneBuildAvailable` to every picker/settings call. |
| `native/tools/probe_emutargets.cpp` | Third argument on every call; new coverage for the gate and the picker order. |
| `native/tools/probe_syscatalog.cpp` | Pins `n64`'s new `externalEmulator`. |
| `native/CMakeLists.txt`, `native/tools/run-headless-probes.sh`, `.github/workflows/ci.yml` | Register `probe_aresinput`. |

---

### Task 1: The `standaloneAvailable` platform gate

`GameLauncher::open` refuses every standalone system on Android and iOS. Before `n64` can become standalone, the pure model must degrade such a system back to its libretro cores where those exist. No behaviour changes on desktop in this task.

**Files:**
- Modify: `native/src/core/EmulationTarget.h`
- Modify: `native/src/ui/MainWindow.cpp`
- Modify: `native/src/launch/GameLauncher.cpp`
- Test: `native/tools/probe_emutargets.cpp`

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces:
  - `constexpr bool kStandaloneBuildAvailable` (in `EmulationTarget.h`) — false under `Q_OS_ANDROID` / `Q_OS_IOS`, true elsewhere.
  - `QList<EmulationTarget> emulationTargetsFor(const GameSystem* sys, bool retroParkAvailable, bool standaloneAvailable)`
  - `EmulationTarget resolveEmulationTarget(const GameSystem* sys, const LaunchOpts::Override& ov, const QString& perSystemCore, const QString& perSystemEmulator, EmuBackend perSystemBackend, bool retroParkAvailable, bool standaloneAvailable)`
  - `ResolvedLaunch resolveLaunch(const GameSystem* sys, const LaunchOpts::Override& ov, const QString& perSystemCore, const QString& perSystemEmulator, EmuBackend perSystemBackend, bool retroParkAvailable, bool dolphinVehiclePresent, bool standaloneAvailable)`

- [ ] **Step 1: Write the failing assertions**

Append this block to `native/tools/probe_emutargets.cpp`, immediately before the final `if (failures == 0)` line. It uses `psx` and `n64`, so fetch those systems at the top of the block.

```cpp
    // ---- 6. standaloneAvailable: the platform gate for a build that cannot spawn an external emulator
    //         (Android / iOS). A standalone system WITH libretro cores degrades to Libretro; one WITHOUT
    //         cores (gc/3ds/nds declare none) stays Standalone and keeps the launcher's "not supported"
    //         message. Hand-computed from the SystemCatalog built-in table.
    {
        const GameSystem* psx = SystemCatalog::byId(QStringLiteral("psx"));
        const GameSystem* n64 = SystemCatalog::byId(QStringLiteral("n64"));
        CHECK(psx != nullptr);
        CHECK(n64 != nullptr);
        if (psx && n64)
        {
            const Override empty;

            // psx has cores { swanstation, mednafen_psx_hw, pcsx_rearmed } -> degrades to cores[0].
            const ResolvedLaunch rp = resolveLaunch(psx, empty, QString(), QString(), EmuBackend::Libretro,
                                                    /*retroParkAvailable*/false, /*vehicle*/false,
                                                    /*standaloneAvailable*/false);
            CHECK(rp.engine == EmuEngine::Libretro);
            CHECK(rp.core == QStringLiteral("swanstation"));
            CHECK(rp.externalEmulatorId.isEmpty());

            // gc declares NO cores -> nothing to degrade to, so it stays Standalone.
            const ResolvedLaunch rg = resolveLaunch(gc, empty, QString(), QString(), EmuBackend::Libretro,
                                                    /*retroParkAvailable*/false, /*vehicle*/false,
                                                    /*standaloneAvailable*/false);
            CHECK(rg.engine == EmuEngine::Standalone);
            CHECK(rg.externalEmulatorId == QStringLiteral("dolphin"));

            // standaloneAvailable=true is byte-for-byte today: psx still resolves to its emulator.
            const ResolvedLaunch rpOn = resolveLaunch(psx, empty, QString(), QString(), EmuBackend::Libretro,
                                                      /*retroParkAvailable*/false, /*vehicle*/false,
                                                      /*standaloneAvailable*/true);
            CHECK(rpOn.engine == EmuEngine::Standalone);
            CHECK(rpOn.externalEmulatorId == QStringLiteral("duckstation"));

            // The gate also removes the standalone target from the OFFERED list, so the picker never shows
            // a target prepareCore would degrade away. psx: [standalone:duckstation, libretro x3] with the
            // gate on; the standalone entry is gone with it off.
            const QList<EmulationTarget> off = emulationTargetsFor(psx, /*retroParkAvailable*/false,
                                                                   /*standaloneAvailable*/false);
            for (const EmulationTarget& t : off) CHECK(t.engine != EmuEngine::Standalone);

            // And the CURRENT-VALUE display matches: with the gate off, psx displays its libretro core.
            const EmulationTarget cur = resolveEmulationTarget(psx, empty, QString(), QString(),
                                                               EmuBackend::Libretro, /*retroParkAvailable*/false,
                                                               /*standaloneAvailable*/false);
            CHECK(cur.engine == EmuEngine::Libretro);
            CHECK(cur.id == QStringLiteral("libretro:swanstation"));
        }
    }
```

Then add `/*standaloneAvailable=*/true` as the final argument to **every** pre-existing `emulationTargetsFor(`, `resolveEmulationTarget(` and `resolveLaunch(` call already in the file, so today's assertions keep asserting today's behaviour.

- [ ] **Step 2: Run it and verify it fails to compile**

```bash
export PATH="/c/Qt/6.8.3/msvc2022_64/bin:/c/mpv-dev:$PATH" && cmake --build build --config Release --target probe_emutargets
```

Expected: FAIL — too many arguments to `emulationTargetsFor` / `resolveEmulationTarget` / `resolveLaunch`.

- [ ] **Step 3: Add the platform constant to `EmulationTarget.h`**

Insert immediately after the `enum class EmuEngine` declaration:

```cpp
// The PLATFORM gate for the standalone engine: can this build spawn an external emulator process at all?
// False on Android and iOS, whose sandboxes cannot launch a downloaded desktop executable — GameLauncher::open
// refuses every standalone system there. Callers pass this as the `standaloneAvailable` argument below; the
// pure functions take a plain bool (NOT this macro) precisely so probe_emutargets can enumerate BOTH values.
#if defined(Q_OS_ANDROID) || defined(Q_OS_IOS)
inline constexpr bool kStandaloneBuildAvailable = false;
#else
inline constexpr bool kStandaloneBuildAvailable = true;
#endif
```

- [ ] **Step 4: Thread the parameter through the three functions**

In `emulationTargetsFor`, change the signature to
`inline QList<EmulationTarget> emulationTargetsFor(const GameSystem* sys, bool retroParkAvailable, bool standaloneAvailable)`
and guard the standalone branch. The `else` block becomes:

```cpp
    else if (standaloneAvailable)
    {
        // The system's own default emulator leads; bound registry emulators follow in registry order, de-duped.
        QStringList ids;
        ids << sys->externalEmulator;
        for (const ExternalEmulator& e : EmulatorRegistry::all())
            if (e.systems.contains(sys->id) && !ids.contains(e.id))
                ids << e.id;
        for (const QString& id : ids)
            out.push_back(EmulationTargets::standalone(id));
    }
```

In `resolveEmulationTarget`, add `, bool standaloneAvailable` to the signature and change the standalone branch's condition:

```cpp
    // Not RetroPark (or clamped away because the system does not support it): the underlying engine's default.
    // The standalone arm is skipped entirely where this build cannot spawn an external emulator — the system
    // then displays the libretro core it will actually launch on (see resolveLaunch's matching degrade).
    if (!sys->externalEmulator.isEmpty() && standaloneAvailable)
```

In `resolveLaunch`, add `, bool standaloneAvailable` as the final parameter, and insert this immediately after the existing `r.engine = engine;` line — before the `switch (engine)`:

```cpp
    // The PLATFORM gate: a build that cannot spawn an external emulator degrades a standalone system to its
    // libretro cores. Only where the system HAS cores — gc / 3ds / nds declare none, so they stay Standalone
    // and the launcher surfaces its existing "isn't supported on Android" message unchanged.
    if (engine == EmuEngine::Standalone && !standaloneAvailable && !sys->cores.isEmpty())
        engine = EmuEngine::Libretro;
    r.engine = engine;
```

(Delete the original `r.engine = engine;` so it is assigned exactly once, after the degrade.)

Update the doc comment above `resolveLaunch` to list the third gate alongside `retroParkAvailable` and `dolphinVehiclePresent`.

- [ ] **Step 5: Update every call site**

MainWindow has 14 calls ending in `kRetroParkBuildAvailable)` and one `resolveLaunch` ending in `kRetroParkBuildAvailable, dolphinVehiclePresent)`. The two `static constexpr bool kRetroParkBuildAvailable = …;` definition lines do not match either pattern, so a single substitution is safe:

```bash
sed -i 's/kRetroParkBuildAvailable)/kRetroParkBuildAvailable, kStandaloneBuildAvailable)/g; s/kRetroParkBuildAvailable, dolphinVehiclePresent)/kRetroParkBuildAvailable, dolphinVehiclePresent, kStandaloneBuildAvailable)/g' native/src/ui/MainWindow.cpp
```

Verify the count changed by exactly 15 and that the two definitions are untouched:

```bash
grep -c "kStandaloneBuildAvailable" native/src/ui/MainWindow.cpp && grep -n "static constexpr bool kRetroParkBuildAvailable" native/src/ui/MainWindow.cpp
```

Expected: `15`, then the two definition lines at 352 and 354 unchanged.

In `native/src/launch/GameLauncher.cpp`, the single `resolveLaunch` call becomes:

```cpp
    const ResolvedLaunch rl = resolveLaunch(sys, ov, Settings::coreFor(sys->id), Settings::emulatorFor(sys->id),
                                            Settings::backendFor(sys->id), retroParkAvailable, dolphinVehiclePresent,
                                            kStandaloneBuildAvailable);
```

- [ ] **Step 6: Build and run the probe**

```bash
cmake --build build --config Release --target probe_emutargets && ./build/Release/probe_emutargets.exe
```

Expected: `EMUTARGETS-OK`.

- [ ] **Step 7: Build the app to prove every call site compiles**

```bash
cmake --build build --config Release --target everythingbox 2>&1 | grep -iE "error|warning C4[0-9]+" | head -20
```

Expected: no `error` lines.

- [ ] **Step 8: Commit**

```bash
git add native/src/core/EmulationTarget.h native/src/ui/MainWindow.cpp native/src/launch/GameLauncher.cpp native/tools/probe_emutargets.cpp
git commit -m "feat: degrade a standalone system to its libretro cores where external emulators are impossible"
```

---

### Task 2: Libretro cores in a standalone system's picker

A standalone system's `cores` are currently unreachable from both the per-game Emulation picker and the per-system Settings list — both read `emulationTargetsFor`. This restores them, giving a route back to `mupen64plus_next` once `n64` flips.

**Files:**
- Modify: `native/src/core/EmulationTarget.h`
- Test: `native/tools/probe_emutargets.cpp`

**Interfaces:**
- Consumes: `emulationTargetsFor(sys, retroParkAvailable, standaloneAvailable)` from Task 1.
- Produces: no signature change — only the contents of the returned list for a standalone system.

- [ ] **Step 1: Write the failing assertions**

Append to `native/tools/probe_emutargets.cpp`, before the final `if (failures == 0)`:

```cpp
    // ---- 7. A STANDALONE system also offers its libretro cores, after its emulators and before RetroPark.
    //         psx: [standalone:duckstation, libretro:swanstation, libretro:mednafen_psx_hw,
    //               libretro:pcsx_rearmed]. RetroPark does not support psx, so no retropark entry.
    //         Hand-computed from the SystemCatalog built-in psx row.
    {
        const GameSystem* psx = SystemCatalog::byId(QStringLiteral("psx"));
        CHECK(psx != nullptr);
        if (psx)
        {
            const QList<EmulationTarget> t = emulationTargetsFor(psx, /*retroParkAvailable*/true,
                                                                 /*standaloneAvailable*/true);
            CHECK(t.size() == 4);
            if (t.size() == 4)
            {
                CHECK(t[0].id == QStringLiteral("standalone:duckstation"));
                CHECK(t[0].displayName == QStringLiteral("DuckStation (standalone)"));
                CHECK(t[1].id == QStringLiteral("libretro:swanstation"));
                CHECK(t[1].displayName == QStringLiteral("swanstation (libretro)"));
                CHECK(t[2].id == QStringLiteral("libretro:mednafen_psx_hw"));
                CHECK(t[3].id == QStringLiteral("libretro:pcsx_rearmed"));
            }
            // gc declares no cores, so it is unchanged: [standalone:dolphin, retropark].
            const QList<EmulationTarget> tg = emulationTargetsFor(gc, /*retroParkAvailable*/true,
                                                                  /*standaloneAvailable*/true);
            CHECK(tg.size() == 2);
            if (tg.size() == 2)
            {
                CHECK(tg[0].id == QStringLiteral("standalone:dolphin"));
                CHECK(tg[1].id == QStringLiteral("retropark"));
            }
            // With the platform gate off, psx offers ONLY its cores.
            const QList<EmulationTarget> tOff = emulationTargetsFor(psx, /*retroParkAvailable*/true,
                                                                    /*standaloneAvailable*/false);
            CHECK(tOff.size() == 3);
            if (tOff.size() == 3) CHECK(tOff[0].id == QStringLiteral("libretro:swanstation"));
        }
    }
```

- [ ] **Step 2: Run it and verify it fails**

```bash
cmake --build build --config Release --target probe_emutargets && ./build/Release/probe_emutargets.exe
```

Expected: FAIL — `EMUTARGETS-FAIL t.size() == 4` (the list is currently 1 entry).

- [ ] **Step 3: Implement**

In `emulationTargetsFor`, after the `if (sys->externalEmulator.isEmpty()) { … } else if (standaloneAvailable) { … }` chain and before the RetroPark push, add:

```cpp
    // A STANDALONE system also offers its libretro cores, so a user can move one game (or the whole system)
    // back onto the in-process tier from the picker — and so the target list names the same cores the
    // platform gate above degrades to. A libretro system already listed its cores in the first branch.
    if (!sys->externalEmulator.isEmpty())
        for (const QString& core : sys->cores)
            out.push_back(EmulationTargets::libretro(core));
```

Update the function's doc comment: the standalone bullet now reads "…de-duped; THEN one target per candidate core (`cores[i]`); THEN — if RetroPark supports the system…".

- [ ] **Step 4: Run the probe**

```bash
cmake --build build --config Release --target probe_emutargets && ./build/Release/probe_emutargets.exe
```

Expected: `EMUTARGETS-OK`.

- [ ] **Step 5: Commit**

```bash
git add native/src/core/EmulationTarget.h native/tools/probe_emutargets.cpp
git commit -m "feat: a standalone system's libretro cores are selectable in the emulation picker"
```

---

### Task 3: The ares registry entry

**Files:**
- Modify: `native/src/core/EmulatorRegistry.h`
- Modify: `native/src/core/EmulatorManager.cpp:88-98` (`assetMatches`)
- Test: `native/tools/probe_useremulators.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces: `EmulatorRegistry::byId("ares")` resolves to the entry below; `sys->externalEmulator == "ares"` becomes valid in Task 5.

**Reference — verified ares v148 facts this entry encodes:**
- CLI is `ares [options]... game(s)`; the system is auto-detected from the ROM, so no `--system` (which the space-split `argsTemplate` could not quote anyway).
- `--no-file-prompt` is mandatory: `Nintendo64::load()` opens a blocking file dialog for any cart with the `dd` (64DD) or `tpak` attribute without it.
- Release assets are `ares-windows-x64.zip`, `ares-windows-clang-cl-arm64.zip`, `ares-macos-universal.zip`, plus `-PDBs.zip` / `-dSYMs.zip`. **There is no Linux binary** — Linux is Flathub `dev.ares.ares`.

- [ ] **Step 1: Write the failing assertions**

Append to `native/tools/probe_useremulators.cpp`, before its final sentinel print:

```cpp
    // ---- ares: the N64 built-in. Pins the fields the launch path and the installer actually read, and the
    //      round-trip through the #52 JSON schema. Hand-computed from the documented ares v148 CLI + assets.
    {
        const ExternalEmulator* a = EmulatorRegistry::byId(QStringLiteral("ares"));
        CHECK(a != nullptr);
        if (a)
        {
            CHECK(a->displayName == QStringLiteral("ares"));
            // --no-file-prompt is load-bearing: without it a 64DD- or Transfer-Pak-capable cart opens a
            // blocking file dialog on load and the launch never reaches gameplay.
            CHECK(a->argsTemplate == QStringLiteral("{fs} --no-file-prompt {rom}"));
            CHECK(a->fullscreenArgs == QStringLiteral("--fullscreen"));
            CHECK(a->windowedArgs.isEmpty());
            // Artifact markers are FULL filenames: a short "macos-universal" marker would match
            // ares-macos-universal-dSYMs.zip, which the release lists FIRST.
            CHECK(a->winArtifact == QStringLiteral("ares-windows-x64.zip"));
            CHECK(a->macArtifact == QStringLiteral("ares-macos-universal.zip"));
            CHECK(a->linuxArtifact.isEmpty());
            CHECK(a->flatpakAppId == QStringLiteral("dev.ares.ares"));
            CHECK(a->systems == QStringList{ QStringLiteral("n64") });
            // hasInstallSource is a FREE function in EmulatorRegistry, not a member of ExternalEmulator.
            CHECK(hasInstallSource(*a));
            CHECK(fromJson(toJson(*a)) == *a);
        }
    }
```

`toJson` / `fromJson` / `hasInstallSource` are the spellings already used in that file (`EmulatorRegistry.h:410`, `:492`, `:497`), so no new `using` is needed.

- [ ] **Step 2: Run it and verify it fails**

```bash
cmake --build build --config Release --target probe_useremulators && ./build/Release/probe_useremulators.exe
```

Expected: FAIL — `USEREMU-FAIL a != nullptr`.

- [ ] **Step 3: Add the entry**

Append to the `builtinEmulators()` initializer list in `native/src/core/EmulatorRegistry.h`, after the last existing entry (`teknoparrot`). Field order follows the struct exactly.

```cpp
            {
                // Nintendo 64. ares is a multi-system accuracy emulator; EverythingBox wires only its N64 core.
                // CLI is "ares [options]... game" and the system is auto-detected from the ROM, so no --system
                // (which this space-split argsTemplate could not quote anyway). --no-file-prompt is NOT optional:
                // any cart carrying the 64DD ("dd") or Transfer Pak ("tpak") attribute opens a BLOCKING file
                // dialog on load without it. No BIOS entry: ares compiles the PIF ROMs in as build resources.
                // Windows is portable by default — ares looks for settings.bml beside its own exe first.
                QStringLiteral("ares"), QStringLiteral("ares"),
                QStringLiteral("{fs} --no-file-prompt {rom}"),
                QStringLiteral("--fullscreen"),   // fullscreenArgs
                QString(),                        // windowedArgs (default is windowed)
                QStringLiteral("https://ares-emu.net/"),
                { QStringLiteral("ares.exe"), QStringLiteral("ares/ares.exe") },
                { QStringLiteral("ares.app/Contents/MacOS/ares"), QStringLiteral("ares.app") },
                { QStringLiteral("ares") },
                QStringLiteral("https://api.github.com/repos/ares-emulator/ares/releases/latest"),
                // FULL filenames, not the usual short platform marker: the release also publishes
                // ares-windows-x64-PDBs.zip and ares-macos-universal-dSYMs.zip, and the dSYMs archive is
                // listed BEFORE the real one in the assets array.
                QStringLiteral("ares-windows-x64.zip"),
                QStringLiteral("ares-macos-universal.zip"),
                QString(),                        // linuxArtifact — ares publishes no Linux binary
                QStringLiteral("dev.ares.ares"),  // Linux build is a Flatpak
                QString(), QString(), QString(),  // win/mac/linux update URL overrides — none
                { QStringLiteral("n64"), QStringLiteral("z64"), QStringLiteral("v64"), QStringLiteral("ndd") },
                { QStringLiteral("n64") },
            },
```

- [ ] **Step 4: Harden `assetMatches` against debug-symbol archives**

In `native/src/core/EmulatorManager.cpp`, line 92, add `"dsym"` to the exclusion list:

```cpp
    for (const char* s : { "libretro", "symbols", "dsym", "dbg", "pdb", "unsigned", "dev" })
```

`"symbols"` does not cover `dSYMs`, and no shipped emulator wants an artifact whose name contains `dsym`, so this can only ever exclude a debug-symbol archive.

- [ ] **Step 5: Run the probe**

```bash
cmake --build build --config Release --target probe_useremulators && ./build/Release/probe_useremulators.exe
```

Expected: `USEREMU-OK`.

- [ ] **Step 6: Commit**

```bash
git add native/src/core/EmulatorRegistry.h native/src/core/EmulatorManager.cpp native/tools/probe_useremulators.cpp
git commit -m "feat: add ares as a standalone emulator"
```

---

### Task 4: `AresInput.h` — translating a pad into ares bindings

ares ships with **no** input bindings at all: no auto-map, no keyboard default, no first-run assignment. A fresh install boots a game you cannot control. This is the pure half of fixing that.

**Files:**
- Create: `native/src/core/AresInput.h`
- Modify: `native/src/core/ControllerSeats.h` (`PadInfo` gains `sdlMapping`)
- Create: `native/tools/probe_aresinput.cpp`
- Modify: `native/CMakeLists.txt`, `native/tools/run-headless-probes.sh`, `.github/workflows/ci.yml`

**Interfaces:**
- Consumes: `ControllerSeats::PadInfo`, `ControllerSeats::Seat`, `ControllerSeats::assignSeats`.
- Produces:
  - `struct AresInput::Binding { QString key; QString value; }` — `key` is the LEAF settings name (`Pad.Up`), not the full path.
  - `QVector<AresInput::Binding> AresInput::bindingsFor(const ControllerSeats::PadInfo& pad)`
  - `QByteArray AresInput::settingsBml(const QVector<ControllerSeats::Seat>& seats)`
  - `bool AresInput::needsSeed(const QByteArray& existingSettingsBml)`
  - `QByteArray AresInput::mergeSettingsBml(const QByteArray& existing, const QByteArray& seed)`

**Why a merge and not an append.** ares resolves a settings path with `Markup::Node::operator[]`, which is `_lookup` → `_find(path)[0]` — the **first** match (`nall/nall/string/markup/find.hpp`). A second `VirtualPad1` block appended after an existing one is therefore silently ignored on load, and `Settings::save`'s `operator()(path).setValue(…)` writes back into the *first* block, so the appended seed would look correct in the file and do nothing. `mergeSettingsBml` strips any existing top-level `VirtualPad{N}` block before appending ours, so exactly one remains.

**Reference — the ares format, verified against `ares-emulator/ares@master`:**

`desktop-ui/settings/settings.cpp` binds each virtual-pad input to `VirtualPad{1..5}/{name}`, where `name` is the display name pushed through `.replace(" ", ".").replace("(", ".").replace(")", "")`. From `VirtualPad::VirtualPad()`:

| ares display name | settings leaf |
|---|---|
| `Pad Up` / `Pad Down` / `Pad Left` / `Pad Right` | `Pad.Up` / `Pad.Down` / `Pad.Left` / `Pad.Right` |
| `A (South)` / `B (East)` / `X (West)` / `Y (North)` | `A..South` / `B..East` / `X..West` / `Y..North` |
| `Select` / `Start` | `Select` / `Start` |
| `L-Bumper` / `R-Bumper` / `L-Trigger` / `R-Trigger` | unchanged |
| `L-Stick (Click)` / `R-Stick (Click)` | `L-Stick..Click` / `R-Stick..Click` |
| `L-Up` / `L-Down` / `L-Left` / `L-Right`, `R-Up` … | unchanged |

An assignment value is `<identity>/<slot>/<groupID>/<inputID>[/<qualifier>]`, parsed by `InputMapping::bind()`. `identity` is the SDL GUID string, `slot` disambiguates two identical pads. Group IDs from `nall/nall/hid.hpp`: `Axis = 0, Hat = 1, Trigger = 2, Button = 3`. `inputID` is the **raw joystick** index — `ruby/input/joypad/sdl.cpp` enumerates `SDL_GetNumJoystickAxes/Hats/Buttons`, not the gamepad abstraction.

Hat polarity, from that same poll loop — each SDL hat becomes two ares hat inputs:

```
assign(Hat, 2H + 0, LEFT ? -32767 : RIGHT ? +32767 : 0);   // X
assign(Hat, 2H + 1, UP   ? -32767 : DOWN  ? +32767 : 0);   // Y
```

so `dpup` → input `2H+1` qualifier `Lo`, `dpdown` → `2H+1` `Hi`, `dpleft` → `2H` `Lo`, `dpright` → `2H` `Hi`. `InputDigital::value()` thresholds a non-Button group at `< -16384` (Lo) / `> +16384` (Hi), so a digital control bound to a hat or axis works.

The file is BML written by `BML::serialize(*this, " ")`: two spaces of indent per depth, `name: value`, LF newlines.

- [ ] **Step 1: Add `sdlMapping` to `PadInfo`**

In `native/src/core/ControllerSeats.h`, extend the struct:

```cpp
    struct PadInfo
    {
        int     index = 0;   // connection-order index among game controllers (0..)
        QString guid;        // SDL joystick GUID — the identity ares keys its bindings on (see AresInput.h)
        QString name;        // human controller name — reserved (diagnostics / future seat UI)
        QString sdlMapping;  // this pad's SDL gamepad mapping string ("<guid>,<name>,a:b0,leftx:a0,…"), the
                             // source of the RAW button/axis indices AresInput translates into ares bindings.
                             // Empty when SDL has no mapping for the device.
    };
```

`PadInfo` has no `operator==`; `Seat::operator==` compares `index` / `pad.index` / `pad.guid` / `pad.name` and is deliberately left alone, so `probe_seats` is unaffected.

- [ ] **Step 2: Write the failing probe**

Create `native/tools/probe_aresinput.cpp`:

```cpp
// Headless check of the pure ares input model (src/core/AresInput.h) — the translation from a pad's SDL
// gamepad mapping string into the ares settings.bml bindings EmulatorManager seeds on first launch. ares
// ships with NO input bindings at all (no auto-map, no keyboard default, no first-run assignment anywhere in
// desktop-ui/input/), so without this seed a fresh install boots N64 games that cannot be controlled.
//
// Qt6::Core only, no SDL, no disk. Prints ARESINPUT-OK on success; any failure prints
// ARESINPUT-FAIL <cond> (line) and exits non-zero.
//
// FIXTURES ARE REAL AND HAND-COMPUTED: the three mapping strings below are copied verbatim from the repo's
// own native/gamecontrollerdb.txt, and every expected assignment is derived by hand from the documented ares
// rules (group ids Axis=0 Hat=1 Trigger=2 Button=3; hat H -> inputs 2H (X, LEFT=Lo/RIGHT=Hi) and 2H+1
// (Y, UP=Lo/DOWN=Hi)) — never by re-running the function under test.
#include "AresInput.h"
#include "ControllerSeats.h"

#include <QCoreApplication>
#include <QString>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "ARESINPUT-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

// Real lines from native/gamecontrollerdb.txt.
// Hat d-pad, AXIS triggers (lefttrigger:a3, righttrigger:a4).
static const char* kPs5 =
    "030000004c050000e60c000000000000,PS5 Controller,a:b1,b:b2,back:b8,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,"
    "dpup:h0.1,guide:b12,leftshoulder:b4,leftstick:b10,lefttrigger:a3,leftx:a0,lefty:a1,misc1:b14,"
    "rightshoulder:b5,rightstick:b11,righttrigger:a4,rightx:a2,righty:a5,start:b9,touchpad:b13,x:b0,y:b3,"
    "platform:Windows,";
// BUTTON d-pad, BUTTON triggers (dpup:b10, lefttrigger:b8).
static const char* k4Play =
    "03000000d0160000040d000000000000,4Play Adapter,a:b1,b:b3,back:b4,dpdown:b11,dpleft:b12,dpright:b13,"
    "dpup:b10,leftshoulder:b6,leftstick:b14,lefttrigger:b8,leftx:a0,lefty:a1,rightshoulder:b7,rightstick:b15,"
    "righttrigger:b9,rightx:a3,righty:a4,start:b5,x:b0,y:b2,platform:Windows,";
// Deliberately incomplete: no right stick, no triggers, no stick clicks.
static const char* kPartial =
    "03000000ffff0000ffff000000000000,Partial Pad,a:b0,b:b1,x:b2,y:b3,back:b6,start:b7,"
    "dpup:h0.1,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,leftx:a0,lefty:a1,platform:Windows,";

static QString valueFor(const QVector<AresInput::Binding>& bs, const char* key)
{
    for (const AresInput::Binding& b : bs) if (b.key == QLatin1String(key)) return b.value;
    return QString();
}
static bool has(const QVector<AresInput::Binding>& bs, const char* key)
{
    for (const AresInput::Binding& b : bs) if (b.key == QLatin1String(key)) return true;
    return false;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // ---- 1. Hat d-pad + axis triggers (PS5). ------------------------------------------------------------
    {
        ControllerSeats::PadInfo p;
        p.index = 0;
        p.guid = QStringLiteral("030000004c050000e60c000000000000");
        p.name = QStringLiteral("PS5 Controller");
        p.sdlMapping = QLatin1String(kPs5);
        const QVector<AresInput::Binding> b = AresInput::bindingsFor(p);
        const QString g = p.guid + QStringLiteral("/0/");

        // Face buttons: SDL a:b1 -> ares "A..South" on Button (group 3) index 1.
        CHECK(valueFor(b, "A..South") == g + QStringLiteral("3/1"));
        CHECK(valueFor(b, "B..East")  == g + QStringLiteral("3/2"));
        CHECK(valueFor(b, "X..West")  == g + QStringLiteral("3/0"));
        CHECK(valueFor(b, "Y..North") == g + QStringLiteral("3/3"));
        CHECK(valueFor(b, "Select")   == g + QStringLiteral("3/8"));
        CHECK(valueFor(b, "Start")    == g + QStringLiteral("3/9"));
        CHECK(valueFor(b, "L-Bumper") == g + QStringLiteral("3/4"));
        CHECK(valueFor(b, "R-Bumper") == g + QStringLiteral("3/5"));
        CHECK(valueFor(b, "L-Stick..Click") == g + QStringLiteral("3/10"));
        CHECK(valueFor(b, "R-Stick..Click") == g + QStringLiteral("3/11"));

        // D-pad on hat 0: X is ares hat input 0 (LEFT=Lo, RIGHT=Hi), Y is input 1 (UP=Lo, DOWN=Hi).
        CHECK(valueFor(b, "Pad.Up")    == g + QStringLiteral("1/1/Lo"));
        CHECK(valueFor(b, "Pad.Down")  == g + QStringLiteral("1/1/Hi"));
        CHECK(valueFor(b, "Pad.Left")  == g + QStringLiteral("1/0/Lo"));
        CHECK(valueFor(b, "Pad.Right") == g + QStringLiteral("1/0/Hi"));

        // Axis triggers: a digital control bound to an axis takes the Hi half.
        CHECK(valueFor(b, "L-Trigger") == g + QStringLiteral("0/3/Hi"));
        CHECK(valueFor(b, "R-Trigger") == g + QStringLiteral("0/4/Hi"));

        // Sticks: SDL X negative = left, Y negative = up.
        CHECK(valueFor(b, "L-Left")  == g + QStringLiteral("0/0/Lo"));
        CHECK(valueFor(b, "L-Right") == g + QStringLiteral("0/0/Hi"));
        CHECK(valueFor(b, "L-Up")    == g + QStringLiteral("0/1/Lo"));
        CHECK(valueFor(b, "L-Down")  == g + QStringLiteral("0/1/Hi"));
        CHECK(valueFor(b, "R-Left")  == g + QStringLiteral("0/2/Lo"));
        CHECK(valueFor(b, "R-Right") == g + QStringLiteral("0/2/Hi"));
        CHECK(valueFor(b, "R-Up")    == g + QStringLiteral("0/5/Lo"));
        CHECK(valueFor(b, "R-Down")  == g + QStringLiteral("0/5/Hi"));
    }

    // ---- 2. Button d-pad + button triggers (4Play). Proves indices come from the mapping string, not an
    //         assumed layout: this pad's A is b1 and its X is b0, the opposite way round from an Xbox pad.
    {
        ControllerSeats::PadInfo p;
        p.index = 0;
        p.guid = QStringLiteral("03000000d0160000040d000000000000");
        p.sdlMapping = QLatin1String(k4Play);
        const QVector<AresInput::Binding> b = AresInput::bindingsFor(p);
        const QString g = p.guid + QStringLiteral("/0/");

        CHECK(valueFor(b, "Pad.Up")    == g + QStringLiteral("3/10"));
        CHECK(valueFor(b, "Pad.Down")  == g + QStringLiteral("3/11"));
        CHECK(valueFor(b, "Pad.Left")  == g + QStringLiteral("3/12"));
        CHECK(valueFor(b, "Pad.Right") == g + QStringLiteral("3/13"));
        CHECK(valueFor(b, "L-Trigger") == g + QStringLiteral("3/8"));
        CHECK(valueFor(b, "R-Trigger") == g + QStringLiteral("3/9"));
        CHECK(valueFor(b, "R-Up")      == g + QStringLiteral("0/4/Lo"));
        CHECK(valueFor(b, "R-Right")   == g + QStringLiteral("0/3/Hi"));
    }

    // ---- 3. A pad missing controls: those keys are OMITTED, never guessed. ------------------------------
    {
        ControllerSeats::PadInfo p;
        p.index = 0;
        p.guid = QStringLiteral("03000000ffff0000ffff000000000000");
        p.sdlMapping = QLatin1String(kPartial);
        const QVector<AresInput::Binding> b = AresInput::bindingsFor(p);
        CHECK(has(b, "Pad.Up"));
        CHECK(has(b, "L-Up"));
        CHECK(!has(b, "R-Up"));
        CHECK(!has(b, "R-Down"));
        CHECK(!has(b, "L-Trigger"));
        CHECK(!has(b, "R-Trigger"));
        CHECK(!has(b, "L-Stick..Click"));
        // An empty mapping string yields NOTHING at all — degrade, never guess.
        ControllerSeats::PadInfo none;
        none.guid = QStringLiteral("03000000ffff0000ffff000000000000");
        CHECK(AresInput::bindingsFor(none).isEmpty());
    }

    // ---- 4. settingsBml: BML shape (two-space indent, "name: value", LF) and VirtualPad numbering. ------
    {
        ControllerSeats::PadInfo p1;
        p1.index = 0;
        p1.guid = QStringLiteral("030000004c050000e60c000000000000");
        p1.sdlMapping = QLatin1String(kPs5);
        ControllerSeats::PadInfo p2;
        p2.index = 1;
        p2.guid = QStringLiteral("03000000d0160000040d000000000000");
        p2.sdlMapping = QLatin1String(k4Play);
        const QVector<ControllerSeats::Seat> seats = ControllerSeats::assignSeats({ p1, p2 });
        CHECK(seats.size() == 2);
        const QByteArray bml = AresInput::settingsBml(seats);

        CHECK(bml.contains("VirtualPad1\n"));
        CHECK(bml.contains("VirtualPad2\n"));
        CHECK(bml.contains("  A..South: 030000004c050000e60c000000000000/0/3/1\n"));
        CHECK(bml.contains("  Pad.Up: 030000004c050000e60c000000000000/0/1/1/Lo\n"));
        CHECK(bml.contains("  A..South: 03000000d0160000040d000000000000/0/3/1\n"));
        CHECK(!bml.contains("\r"));                       // LF only, as BML::serialize writes
        CHECK(!bml.contains("VirtualPad3"));              // only seated pads are written
        // No pads at all -> nothing to write, so the caller seeds no file.
        CHECK(AresInput::settingsBml({}).isEmpty());
    }

    // ---- 5. needsSeed: only a file with NO VirtualPad assignment is seeded. -----------------------------
    {
        CHECK(AresInput::needsSeed(QByteArray()));                       // absent / empty file
        CHECK(AresInput::needsSeed("Video\n  Driver: Direct3D 11\n"));   // ares ran, never mapped
        // ares writes every VirtualPad key with an empty value when nothing is assigned.
        CHECK(AresInput::needsSeed("VirtualPad1\n  Pad.Up:\n  A..South:\n"));
        // A user's own mapping is never touched.
        CHECK(!AresInput::needsSeed("VirtualPad1\n  Pad.Up: 0300/0/1/1/Lo\n"));
        CHECK(!AresInput::needsSeed("Video\n  Driver: Direct3D 11\nVirtualPad2\n  Start: 0300/0/3/9\n"));
    }

    // ---- 6. mergeSettingsBml: exactly ONE VirtualPadN block survives. ares resolves a settings path with
    //         _find(path)[0] — the FIRST match — so a seed appended after ares' own empty block would be
    //         silently ignored on load AND overwritten on save. Every pre-existing VirtualPad block must go.
    {
        const QByteArray seed = "VirtualPad1\n  Pad.Up: 0300/0/1/1/Lo\n";

        // Absent file: the seed IS the file.
        CHECK(AresInput::mergeSettingsBml(QByteArray(), seed) == seed);

        // ares ran once and wrote an unmapped file: its VirtualPad block is replaced, not duplicated, and
        // every non-pad setting it wrote survives untouched.
        const QByteArray existing =
            "Video\n"
            "  Driver: Direct3D 11\n"
            "VirtualPad1\n"
            "  Pad.Up:\n"
            "  A..South:\n"
            "Audio\n"
            "  Driver: WASAPI\n";
        const QByteArray merged = AresInput::mergeSettingsBml(existing, seed);
        CHECK(merged.count("VirtualPad1") == 1);
        CHECK(merged.contains("  Pad.Up: 0300/0/1/1/Lo\n"));
        CHECK(!merged.contains("  A..South:\n"));           // the stale empty key is gone with its block
        CHECK(merged.contains("Video\n  Driver: Direct3D 11\n"));
        CHECK(merged.contains("Audio\n  Driver: WASAPI\n"));

        // A stale block for a pad we are NOT seeding this time is also removed, so a settings.bml cannot
        // accumulate bindings for a controller that is no longer attached.
        const QByteArray twoPads =
            "VirtualPad1\n  Pad.Up:\n"
            "VirtualPad2\n  Pad.Up:\n"
            "Video\n  Driver: Direct3D 11\n";
        const QByteArray merged2 = AresInput::mergeSettingsBml(twoPads, seed);
        CHECK(!merged2.contains("VirtualPad2"));
        CHECK(merged2.count("VirtualPad1") == 1);
        CHECK(merged2.contains("Video\n  Driver: Direct3D 11\n"));
    }

    if (failures == 0) std::printf("ARESINPUT-OK\n");
    else               std::fprintf(stderr, "ARESINPUT: %d check(s) failed\n", failures);
    return failures == 0 ? 0 : 1;
}
```

- [ ] **Step 3: Register the probe in all three places**

`native/CMakeLists.txt` — add beside the `probe_seats` block (around line 1163):

```cmake
    # ares input seeding: the pure SDL-mapping -> ares settings.bml translation (src/core/AresInput.h,
    # header-only). QtCore-only, no SDL, offscreen-QPA safe.
    add_executable(probe_aresinput tools/probe_aresinput.cpp src/core/AresInput.h src/core/ControllerSeats.h)
    target_include_directories(probe_aresinput PRIVATE src src/core)
    target_link_libraries(probe_aresinput PRIVATE Qt6::Core)
```

`native/tools/run-headless-probes.sh` — this file is CRLF and `sed -i` would rewrite every line ending (confirmed in Task 1). Use a byte-preserving edit:

```bash
python -c "
import io
p='native/tools/run-headless-probes.sh'
b=open(p,'rb').read()
old=b'\"probe_seats SEATS-OK\"'
new=b'\"probe_seats SEATS-OK\" \"probe_aresinput ARESINPUT-OK\"'
assert b.count(old)==1, b.count(old)
open(p,'wb').write(b.replace(old,new))
"
```

Verify the line ending survived and the script still parses:

```bash
grep -c $'\r' native/tools/run-headless-probes.sh && bash -n native/tools/run-headless-probes.sh && grep -o "probe_aresinput ARESINPUT-OK" native/tools/run-headless-probes.sh
```

Expected: a non-zero CR count, no output from `bash -n`, then `probe_aresinput ARESINPUT-OK`.

`.github/workflows/ci.yml` — add the target to the "Build probes" list, the same byte-preserving way:

```bash
python -c "
p='.github/workflows/ci.yml'
b=open(p,'rb').read()
old=b' probe_seats '
assert b.count(old)==1, b.count(old)
open(p,'wb').write(b.replace(old,b' probe_seats probe_aresinput '))
"
```

- [ ] **Step 4: Run it and verify it fails**

```bash
cmake --build build --config Release --target probe_aresinput
```

Expected: FAIL — `Cannot open include file: 'AresInput.h'`.

- [ ] **Step 5: Implement `AresInput.h`**

Create `native/src/core/AresInput.h`:

```cpp
// ares input seeding (the pure heart) — translate a connected pad's SDL gamepad mapping string into the
// bindings ares reads out of its own settings.bml. ares ships with NO input bindings whatsoever: there is no
// auto-map, no keyboard default and no first-run assignment anywhere in its desktop-ui, so a fresh install
// boots a game that cannot be controlled until the user maps sixteen controls by hand. This is the standalone
// twin of the melonDS seed EmulatorManager already performs for the same reason.
//
// PURE, header-only, QtCore-only, NO SDL, NO disk. The write side (EmulatorManager::prepareControllerConfig)
// enumerates live pads via SDL, calls ControllerSeats::assignSeats, and seeds settingsBml() ONLY when
// needsSeed() says no VirtualPad assignment exists — so a user's own mapping is never clobbered. Keeping this
// pure is what lets probe_aresinput pin the whole translation against real gamecontrollerdb lines with no SDL,
// no disk and no live emulator.
//
// THE FORMAT (verified against ares-emulator/ares@master, v148):
//   * settings.bml is BML written by BML::serialize(*this, " ") — two spaces of indent per depth,
//     "name: value", LF newlines.
//   * Each virtual-pad input lives at VirtualPad{1..5}/{name}, where {name} is the input's DISPLAY name
//     pushed through .replace(" ", ".").replace("(", ".").replace(")", "") — so "A (South)" is stored as
//     "A..South" and "L-Stick (Click)" as "L-Stick..Click" (desktop-ui/settings/settings.cpp).
//   * An assignment is "<identity>/<slot>/<groupID>/<inputID>[/<qualifier>]" (InputMapping::bind).
//     `identity` is the SDL GUID string and `slot` disambiguates two pads reporting the SAME GUID
//     (ruby/input/joypad/sdl.cpp sets identifier = {identity, "/", slot}). We always emit slot 0: seats are
//     distinguished by their VirtualPad NUMBER, and ares' own enumeration order is not observable from here.
//   * Group ids are nall's HID::Joypad::GroupID — Axis 0, Hat 1, Trigger 2, Button 3 (nall/nall/hid.hpp).
//     The SDL joypad driver only ever populates Axis, Hat and Button.
//   * inputID is the RAW joystick index, because that driver enumerates SDL_GetNumJoystickAxes/Hats/Buttons
//     rather than the gamepad abstraction. Which is exactly why this translation reads the pad's own SDL
//     mapping string instead of assuming a layout: on a "4Play Adapter" the A button is b1 and X is b0.
//   * Each SDL hat becomes TWO ares hat inputs: 2H is X (LEFT -32767, RIGHT +32767) and 2H+1 is Y
//     (UP -32767, DOWN +32767), so up/left take the Lo qualifier and down/right take Hi.
//   * InputDigital::value() thresholds a non-Button group at < -16384 (Lo) / > +16384 (Hi), so a DIGITAL
//     control bound to a hat or to a trigger axis works.
//
// NOT SEEDED, deliberately: Rumble (its assignment needs a raw input to hang a /Rumble qualifier off, which
// the SDL mapping string does not name) and ares' hotkeys (exit is already EverythingBox's job — the launcher
// asks the process to close and force-kills it after a grace period).
#pragma once
#include <QByteArray>
#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>
#include "ControllerSeats.h"

namespace AresInput
{
    // nall HID::Joypad::GroupID.
    enum Group { GroupAxis = 0, GroupHat = 1, GroupTrigger = 2, GroupButton = 3 };

    // One seeded line: `key` is the LEAF settings name ("Pad.Up"); settingsBml supplies the VirtualPadN parent.
    struct Binding
    {
        QString key;
        QString value;
        bool operator==(const Binding& o) const { return key == o.key && value == o.value; }
    };

    // ---- internal: one SDL mapping value ("b3", "a0", "-a1", "a4~", "h0.1") resolved to a raw input -------
    struct RawRef
    {
        bool valid = false;
        int  group = GroupButton;
        int  input = 0;
        int  hatMask = 0;   // hats only: 1 up, 2 right, 4 down, 8 left
    };

    // Parse "a:b1,leftx:a0,dpup:h0.1,platform:Windows," (after the leading GUID and name fields) into a map.
    inline QHash<QString, QString> parseMapping(const QString& sdlMapping)
    {
        QHash<QString, QString> out;
        const QStringList fields = sdlMapping.split(QLatin1Char(','), Qt::SkipEmptyParts);
        // Field 0 is the GUID and field 1 the device name; neither is a key:value pair.
        for (int i = 2; i < fields.size(); ++i)
        {
            const int colon = fields[i].indexOf(QLatin1Char(':'));
            if (colon <= 0) continue;
            const QString k = fields[i].left(colon).trimmed();
            const QString v = fields[i].mid(colon + 1).trimmed();
            if (k.isEmpty() || v.isEmpty() || k == QLatin1String("platform")) continue;
            out.insert(k, v);
        }
        return out;
    }

    inline RawRef refFor(const QHash<QString, QString>& map, const char* sdlName)
    {
        RawRef r;
        const QString v = map.value(QLatin1String(sdlName));
        if (v.isEmpty()) return r;
        // SDL decorates an axis with a half-axis prefix (+/-) and/or an inversion suffix (~). Neither changes
        // WHICH raw axis it is, and the direction we want is decided per key below, so strip them.
        QString s = v;
        while (!s.isEmpty() && (s[0] == QLatin1Char('+') || s[0] == QLatin1Char('-'))) s.remove(0, 1);
        while (s.endsWith(QLatin1Char('~'))) s.chop(1);
        if (s.size() < 2) return r;
        const QChar kind = s[0];
        const QString rest = s.mid(1);
        bool ok = false;
        if (kind == QLatin1Char('b'))
        {
            const int n = rest.toInt(&ok);
            if (!ok || n < 0) return r;
            r.valid = true; r.group = GroupButton; r.input = n;
        }
        else if (kind == QLatin1Char('a'))
        {
            const int n = rest.toInt(&ok);
            if (!ok || n < 0) return r;
            r.valid = true; r.group = GroupAxis; r.input = n;
        }
        else if (kind == QLatin1Char('h'))
        {
            const int dot = rest.indexOf(QLatin1Char('.'));
            if (dot <= 0) return r;
            bool ok2 = false;
            const int hat  = rest.left(dot).toInt(&ok);
            const int mask = rest.mid(dot + 1).toInt(&ok2);
            if (!ok || !ok2 || hat < 0) return r;
            // Hat H occupies ares inputs 2H (X) and 2H+1 (Y).
            const bool vertical = (mask == 1 || mask == 4);
            r.valid = true; r.group = GroupHat; r.input = hat * 2 + (vertical ? 1 : 0); r.hatMask = mask;
        }
        return r;
    }

    // The qualifier a DIGITAL control needs for this raw input: none for a button, Lo/Hi from the hat
    // direction, Hi for an axis (a trigger rests low and rises).
    inline QString digitalQualifier(const RawRef& r)
    {
        if (r.group == GroupButton) return QString();
        if (r.group == GroupHat)
            return (r.hatMask == 1 || r.hatMask == 8) ? QStringLiteral("Lo") : QStringLiteral("Hi");
        return QStringLiteral("Hi");
    }

    inline QString assignment(const QString& guid, const RawRef& r, const QString& qualifier)
    {
        QString s = guid + QStringLiteral("/0/") + QString::number(r.group) + QLatin1Char('/')
                  + QString::number(r.input);
        if (!qualifier.isEmpty()) s += QLatin1Char('/') + qualifier;
        return s;
    }

    // ---- pure: every binding one pad contributes, in a stable order --------------------------------------
    // A control the pad's mapping does not declare simply gets NO binding — degrade, never guess. An empty
    // mapping string (SDL knows the device but has no gamepad profile for it) yields nothing at all.
    inline QVector<Binding> bindingsFor(const ControllerSeats::PadInfo& pad)
    {
        QVector<Binding> out;
        if (pad.sdlMapping.isEmpty() || pad.guid.isEmpty()) return out;
        const QHash<QString, QString> map = parseMapping(pad.sdlMapping);
        if (map.isEmpty()) return out;

        // Digital controls: ares settings leaf <- SDL control name.
        struct D { const char* key; const char* sdl; };
        static const D kDigital[] = {
            { "Pad.Up",         "dpup"          }, { "Pad.Down",       "dpdown"        },
            { "Pad.Left",       "dpleft"        }, { "Pad.Right",      "dpright"       },
            { "Select",         "back"          }, { "Start",          "start"         },
            { "A..South",       "a"             }, { "B..East",        "b"             },
            { "X..West",        "x"             }, { "Y..North",       "y"             },
            { "L-Bumper",       "leftshoulder"  }, { "R-Bumper",       "rightshoulder" },
            { "L-Trigger",      "lefttrigger"   }, { "R-Trigger",      "righttrigger"  },
            { "L-Stick..Click", "leftstick"     }, { "R-Stick..Click", "rightstick"    },
        };
        for (const D& d : kDigital)
        {
            const RawRef r = refFor(map, d.sdl);
            if (!r.valid) continue;
            out.push_back(Binding{ QLatin1String(d.key), assignment(pad.guid, r, digitalQualifier(r)) });
        }

        // Analog stick directions: one axis, split into its two halves. SDL reports X negative = left and
        // Y negative = up, matching ares' Lo/Hi. A pad that reports a stick as buttons contributes nothing
        // here (r.group != GroupAxis), which is correct: a digital stick is not an analog source.
        struct A { const char* key; const char* sdl; const char* qual; };
        static const A kAnalog[] = {
            { "L-Left",  "leftx",  "Lo" }, { "L-Right", "leftx",  "Hi" },
            { "L-Up",    "lefty",  "Lo" }, { "L-Down",  "lefty",  "Hi" },
            { "R-Left",  "rightx", "Lo" }, { "R-Right", "rightx", "Hi" },
            { "R-Up",    "righty", "Lo" }, { "R-Down",  "righty", "Hi" },
        };
        for (const A& a : kAnalog)
        {
            const RawRef r = refFor(map, a.sdl);
            if (!r.valid || r.group != GroupAxis) continue;
            out.push_back(Binding{ QLatin1String(a.key), assignment(pad.guid, r, QLatin1String(a.qual)) });
        }
        return out;
    }

    // ---- pure: the settings.bml body to seed for a whole seat list ---------------------------------------
    // Seat n becomes VirtualPad{n+1}. A seat whose pad contributes no binding is skipped entirely, and an
    // empty seat list yields an empty body — the caller then seeds no file at all.
    inline QByteArray settingsBml(const QVector<ControllerSeats::Seat>& seats)
    {
        QByteArray out;
        for (const ControllerSeats::Seat& s : seats)
        {
            const QVector<Binding> bs = bindingsFor(s.pad);
            if (bs.isEmpty()) continue;
            out += QStringLiteral("VirtualPad%1\n").arg(s.index + 1).toUtf8();
            for (const Binding& b : bs)
                out += "  " + b.key.toUtf8() + ": " + b.value.toUtf8() + "\n";
        }
        return out;
    }

    // ---- pure: may we seed over this existing settings.bml? ----------------------------------------------
    // True only when NO VirtualPad input carries a non-empty assignment: an absent file, or one ares wrote
    // itself before we ever seeded (it persists every key with an empty value when nothing is mapped). Any
    // single real assignment — a user's own mapping — makes this false and the file is left untouched.
    inline bool needsSeed(const QByteArray& existingSettingsBml)
    {
        const QStringList lines = QString::fromUtf8(existingSettingsBml).split(QLatin1Char('\n'));
        bool inPad = false;
        for (const QString& raw : lines)
        {
            const QString line = raw.trimmed();
            if (line.isEmpty()) continue;
            const bool topLevel = !raw.startsWith(QLatin1Char(' ')) && !raw.startsWith(QLatin1Char('\t'));
            if (topLevel) { inPad = line.startsWith(QStringLiteral("VirtualPad")); continue; }
            if (!inPad) continue;
            const int colon = line.indexOf(QLatin1Char(':'));
            if (colon >= 0 && !line.mid(colon + 1).trimmed().isEmpty()) return false;
        }
        return true;
    }

    // ---- pure: fold a seed into an existing settings.bml -------------------------------------------------
    // MUST NOT be a plain append. ares resolves a settings path with Markup::Node::operator[], which is
    // _lookup -> _find(path)[0] — the FIRST match (nall/nall/string/markup/find.hpp). A second VirtualPad1
    // block appended after one ares already wrote is therefore ignored on load, and Settings::save's
    // operator()(path).setValue() writes back into the FIRST block — so the seed would read perfectly in the
    // file and do nothing at all. Every pre-existing top-level VirtualPad{N} block is dropped (including one
    // for a pad we are not seeding this time, so the file cannot accumulate stale controllers) and the seed
    // appended, leaving exactly one block per seated pad. Every non-pad setting is preserved byte-for-byte.
    // Only ever called behind needsSeed(), so no real assignment can be discarded here.
    inline QByteArray mergeSettingsBml(const QByteArray& existing, const QByteArray& seed)
    {
        if (existing.isEmpty()) return seed;

        QByteArray kept;
        bool dropping = false;
        const QList<QByteArray> lines = existing.split('\n');
        for (int i = 0; i < lines.size(); ++i)
        {
            const QByteArray& raw = lines[i];
            // A trailing "\n" splits to a final empty element; don't re-emit it as a blank line.
            if (i == lines.size() - 1 && raw.isEmpty()) break;
            const bool topLevel = !raw.startsWith(' ') && !raw.startsWith('\t') && !raw.trimmed().isEmpty();
            if (topLevel)
                dropping = raw.trimmed().startsWith("VirtualPad");
            if (dropping) continue;
            kept += raw;
            kept += '\n';
        }
        return kept + seed;
    }
}
```

- [ ] **Step 6: Run the probe**

```bash
cmake --build build --config Release --target probe_aresinput && ./build/Release/probe_aresinput.exe
```

Expected: `ARESINPUT-OK`.

- [ ] **Step 7: Prove the probe actually runs in the suite**

```bash
BUILD_DIR=build bash native/tools/run-headless-probes.sh 2>&1 | grep -E "aresinput|ARESINPUT"
```

Expected: a `PASS:` line naming `probe_aresinput`. If nothing is printed, the runner registration did not take — fix it before committing.

- [ ] **Step 8: Commit**

```bash
git add native/src/core/AresInput.h native/src/core/ControllerSeats.h native/tools/probe_aresinput.cpp native/CMakeLists.txt native/tools/run-headless-probes.sh .github/workflows/ci.yml
git commit -m "feat: translate a connected pad into ares settings.bml bindings"
```

---

### Task 5: Seed `settings.bml` on launch

**Files:**
- Modify: `native/src/core/EmulatorManager.cpp` (`enumerateConnectedPads`, `prepareControllerConfig`)

**Interfaces:**
- Consumes: `AresInput::settingsBml`, `AresInput::needsSeed`, `ControllerSeats::assignSeats`, the existing file-scope `seedFileIfAbsent`.
- Produces: nothing later tasks call.

- [ ] **Step 1: Include the new header**

Add to the include block at the top of `native/src/core/EmulatorManager.cpp`, beside the existing `ControllerSeats.h` include:

```cpp
#include "AresInput.h"            // pure SDL-mapping -> ares settings.bml translation (ares ships unmapped)
```

- [ ] **Step 2: Fill `sdlMapping` when enumerating pads**

In `enumerateConnectedPads`, inside the loop and immediately after `p.name` is set:

```cpp
        if (char* m = SDL_GameControllerMappingForDeviceIndex(i))
        {
            p.sdlMapping = QString::fromUtf8(m);
            SDL_free(m);
        }
```

`SDL_GameControllerMappingForDeviceIndex` returns a heap string the caller frees, exactly like `SDL_GetBasePath` a few lines above. The loop already loads `gamecontrollerdb.txt`, so the mapping matches the in-process tier's.

- [ ] **Step 3: Add the `ares` arm to `prepareControllerConfig`**

Insert immediately before the existing `// ---- melonDS:` comment block:

```cpp
    // ---- ares: ships with EVERY input unmapped — no auto-map, no keyboard default, no first-run assignment
    // anywhere in its desktop-ui — so a fresh install boots N64 games that cannot be controlled. Seed the
    // virtual pads from the live controllers. UNLIKE the four seated emulators above this is NOT per-seat:
    // settings.bml is ONE file covering all four virtual pads, so it is built from the whole seat list in one
    // call. Seeded only when no VirtualPad assignment exists yet (an absent file, or one ares wrote itself
    // before we seeded), so a user's own mapping is never clobbered. ----
    if (id == QStringLiteral("ares"))
    {
        const QVector<ControllerSeats::Seat> seats =
            ControllerSeats::assignSeats(enumerateConnectedPads());
        const QByteArray body = AresInput::settingsBml(seats);
        if (body.isEmpty()) return;   // no pad, or none SDL has a mapping for — leave ares' own config alone
        const QString path = binDir + QStringLiteral("/settings.bml");
        QFile f(path);
        QByteArray existing;
        if (f.exists() && f.open(QIODevice::ReadOnly)) { existing = f.readAll(); f.close(); }
        if (!AresInput::needsSeed(existing)) return;
        // NOT an append: ares resolves a settings path to the FIRST matching node, so a second VirtualPad1
        // block would be ignored on load and overwritten on its next save. mergeSettingsBml drops any
        // pre-existing VirtualPad block and appends ours, preserving every other setting ares wrote.
        const QByteArray merged = AresInput::mergeSettingsBml(existing, body);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        {
            f.write(merged);
            f.close();
        }
        return;
    }
```

`seedFileIfAbsent` is not used here: `mergeSettingsBml` already returns the seed verbatim when the file is absent, and the `needsSeed` gate above is the never-clobber guard.

- [ ] **Step 4: Build the app**

```bash
cmake --build build --config Release --target everythingbox 2>&1 | grep -iE "error" | head -20
```

Expected: no `error` lines.

- [ ] **Step 5: Commit**

```bash
git add native/src/core/EmulatorManager.cpp
git commit -m "feat: seed ares' virtual pads from the connected controllers on first launch"
```

---

### Task 6: Make ares the N64 default

The flip. Everything it depends on is now in place: the platform gate protects Android TV, the picker offers a route back to `mupen64plus_next`, and the input seed makes a fresh install playable.

**Files:**
- Modify: `native/src/core/SystemCatalog.h:88-89`
- Test: `native/tools/probe_syscatalog.cpp`

**Interfaces:**
- Consumes: `EmulatorRegistry::byId("ares")` from Task 3, the gate from Task 1.
- Produces: `SystemCatalog::byId("n64")->externalEmulator == "ares"`.

- [ ] **Step 1: Write the failing assertion**

Append to `native/tools/probe_syscatalog.cpp`, before its final sentinel print:

```cpp
    // ---- n64 runs in standalone ares, and KEEPS its libretro cores: the platform gate degrades to cores[0]
    //      where an external emulator is impossible (Android / iOS), and the picker offers them as targets.
    {
        const GameSystem* n64 = SystemCatalog::byId(QStringLiteral("n64"));
        CHECK(n64 != nullptr);
        if (n64)
        {
            CHECK(n64->externalEmulator == QStringLiteral("ares"));
            CHECK(n64->cores.size() == 2);
            CHECK(n64->cores.value(0) == QStringLiteral("mupen64plus_next"));
            CHECK(n64->cores.value(1) == QStringLiteral("parallel_n64"));
            // ares needs no BIOS — it compiles the PIF ROMs in as build resources.
            CHECK(!BiosCatalog::systemNeedsBios(QStringLiteral("n64")));
            CHECK(BiosCatalog::forExternalEmulator(QStringLiteral("ares")).systemId.isEmpty());
        }
    }
```

`probe_syscatalog.cpp` includes only `SystemCatalog.h` today, so the BIOS assertions need two additions. Add the include beside it:

```cpp
#include "BiosCatalog.h"
```

and extend its source list at `native/CMakeLists.txt:1370`:

```cmake
    add_executable(probe_syscatalog tools/probe_syscatalog.cpp src/core/SystemCatalog.h src/core/BiosCatalog.h)
```

`BiosCatalog.h` is header-only and pulls in nothing beyond `QString` / `QSet`, so this adds no link dependency.

- [ ] **Step 2: Run it and verify it fails**

```bash
cmake --build build --config Release --target probe_syscatalog && ./build/Release/probe_syscatalog.exe
```

Expected: FAIL — `SYSCATALOG-FAIL n64->externalEmulator == QStringLiteral("ares")`.

- [ ] **Step 3: Flip the catalog row**

In `native/src/core/SystemCatalog.h`, replace

```cpp
            { "n64",     "Nintendo 64",                       { "n64", "z64", "v64", "ndd" },
                         { "mupen64plus_next", "parallel_n64" } },
```

with

```cpp
            // Nintendo 64 runs in standalone ares (auto-downloaded). The libretro cores are kept here because
            // they are still reachable three ways: the emulation picker offers them as targets, the platform
            // gate degrades to cores[0] where an external emulator is impossible (Android / iOS), and removing
            // the externalEmulator line restores the in-process path outright.
            { "n64",     "Nintendo 64",                       { "n64", "z64", "v64", "ndd" },
                         { "mupen64plus_next", "parallel_n64" }, "ares" },
```

- [ ] **Step 4: Run the probe**

```bash
cmake --build build --config Release --target probe_syscatalog && ./build/Release/probe_syscatalog.exe
```

Expected: `SYSCATALOG-OK`.

- [ ] **Step 5: Commit**

```bash
git add native/src/core/SystemCatalog.h native/tools/probe_syscatalog.cpp native/CMakeLists.txt
git commit -m "feat: Nintendo 64 defaults to standalone ares"
```

---

### Task 7: Full gate

**Files:** none modified unless the gate turns something red.

- [ ] **Step 1: Rebuild everything the change touches and grep for errors**

```bash
cmake --build build --config Release --target everythingbox probe_emutargets probe_aresinput probe_syscatalog probe_useremulators probe_seats probe_launchopts 2>&1 | grep -iE "error" | head -20
```

Expected: no `error` lines.

- [ ] **Step 2: Run the full probe suite**

```bash
BUILD_DIR=build bash native/tools/run-headless-probes.sh; echo "exit=$?"; cat build/headless-probes.verdict
```

Expected: the run ends `ALL HEADLESS PROBES PASSED`, `exit=0`, and the verdict file reads `VERDICT=PASS`. Do not read the result from a piped `tail` — a pipeline reports the last command's status, not the suite's.

- [ ] **Step 3: If anything is red, fix it before continuing**

A failure names itself in the last line and in the verdict file. The most likely candidates given this change: `probe_emutargets` (a call site missed the third argument), `probe_syscatalog` (the psx row ordering assertions sit near the n64 row), `probe_useremulators` (a relative size assertion), and `bash -n` on the runner if the CRLF substitution went wrong.

- [ ] **Step 4: Commit any fixes**

```bash
git add -A native/ .github/
git commit -m "fix: keep the headless gate green after the ares landing"
```

---

### Task 8: Live verification

The probe suite cannot see any of this. Three things need a real machine, a real pad and a real ROM, and the branch is not done until they are checked. Run the app from a deployed Release build per the project's usual deploy step.

- [ ] **Step 1: Install ares from inside the app**

Launch an N64 game. EverythingBox should fetch `ares-windows-x64.zip` and extract it under `<data>/emulators/ares/`.

Verify the **right** archive landed — this is the `dSYMs` / `PDBs` trap:

```bash
ls -la ~/AppData/Roaming/EverythingBox/emulators/ares/ 2>/dev/null || ls -la "$LOCALAPPDATA/EverythingBox/emulators/ares/"
```

Expected: `ares.exe` present, and no `.pdb`-only tree.

- [ ] **Step 2: Verify the SDL2 / SDL3 GUID match — the one that can silently fail**

EverythingBox links SDL2; ares v148 links SDL3. The seeded `identity` must be byte-identical to what ares computes or every binding is inert while looking perfectly well-formed.

With a pad attached, after the first launch:

```bash
cat ~/AppData/Roaming/EverythingBox/emulators/ares/settings.bml | grep -A 20 "^VirtualPad1"
```

Then, in ares' own Settings ▸ Input panel, confirm the bindings render as a **named device** rather than `(disconnected)` — `InputMapping::Binding::text()` returns `(disconnected)` exactly when the identity failed to resolve to a live device.

If they read `(disconnected)`, the GUID strings differ. The fix is to record the GUID ares itself writes (map one button by hand in its UI and read the value back out of `settings.bml`) and reconcile the two spellings in `enumerateConnectedPads`. **Do not** claim this task complete on a `(disconnected)` reading.

- [ ] **Step 3: Verify the controls actually play**

In a game, check: the analog stick moves smoothly in all directions (not digital), the C-buttons respond on the right stick, Z responds on the right trigger, L/R on the bumpers, and Start starts. A trigger that fires immediately or never means the `Hi` qualifier polarity is wrong for that pad — correct `digitalQualifier` and re-run `probe_aresinput`.

- [ ] **Step 4: Verify `--no-file-prompt` reaches ares intact**

Launch a 64DD-capable cart (any cart whose ares game database entry carries the `dd` attribute). It must boot straight to gameplay with **no** file dialog. A dialog means the flag did not survive the space-split `argsTemplate`.

- [ ] **Step 5: Verify the picker**

On an N64 game, open the per-game Emulation picker. It should list `ares (standalone)`, `mupen64plus_next (libretro)`, `parallel_n64 (libretro)`, and — on a RetroPark build — the RetroPark target. Switch one game to `mupen64plus_next`, confirm it launches in-process, then switch it back.

- [ ] **Step 6: Verify the never-clobber rule**

Remap one control by hand inside ares, quit, and launch an N64 game from EverythingBox again. The hand-made mapping must survive — `needsSeed` returns false the moment any VirtualPad assignment is non-empty.

- [ ] **Step 7: Record the outcome**

Append a short "Live verification" section to the spec recording what was tested, on what pad, and anything that had to be corrected. Commit it.

```bash
git add docs/superpowers/specs/2026-08-30-ares-n64-standalone-design.md
git commit -m "docs: record the ares live-verification pass"
```

---

## Self-Review

**Spec coverage.** Registry entry → Task 3. `dsym` hardening → Task 3. `n64` wiring, no BIOS entry → Task 6. `standaloneAvailable` gate across all three model functions and every call site → Task 1. Cores in a standalone picker → Task 2. `AresInput.h`, `PadInfo.sdlMapping`, the write side → Tasks 4 and 5. `probe_aresinput` with the four fixture shapes, extended `probe_emutargets`, `probe_syscatalog`, `probe_useremulators` → Tasks 1–6. Three-place registration → Task 4 Step 3. Gate green → Task 7. The two things not headlessly verifiable, plus the `--no-file-prompt` check → Task 8. The known limitations in the spec are documented in `AresInput.h`'s header comment rather than coded around, which is what the spec asks for.

**Type consistency.** `bindingsFor(const ControllerSeats::PadInfo&)` (not `bindingsForSeat`) is used identically in the probe, the header and Task 5's caller. `Binding::key` is the leaf name everywhere; `settingsBml` is the only thing that writes a `VirtualPadN` parent. `needsSeed` takes a `QByteArray` in all three places. `kStandaloneBuildAvailable` is spelled the same in `EmulationTarget.h`, `MainWindow.cpp` and `GameLauncher.cpp`. The third parameter is last in all three model signatures, matching the sed in Task 1 Step 5.

**Note on the spec's function naming.** The spec describes the pure header as exposing "one function taking the assigned seats and returning the `settings.bml` body" — that is `settingsBml`. `bindingsFor` and `needsSeed` are the two supporting entry points the probe and the write side need; no design change.
