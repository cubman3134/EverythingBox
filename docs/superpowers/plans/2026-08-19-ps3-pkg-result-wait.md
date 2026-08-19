# PS3 game-update install: wait on the RESULT, not on an RPCS3 exit that never comes

## Problem

`rpcs3.exe --installpkg <pkg>` installs the package and then STAYS OPEN as the normal GUI
(hardware-verified 2026-08-19 with the sibling `--installfw` — same behavior class). The
`--installpkg` installer lambda in `native/src/core/EmulatorManager.cpp` waits on process
exit with a 10-minute bound, so every PS3 launch with pending updates stalls 10 minutes,
kills a SUCCESSFUL install, returns -1, and the update chain aborts — updates are never
recorded as applied (`Ps3UpdateState`) and the stall repeats on every launch.

The firmware twin was already fixed on main (f514390): poll for the RESULT (dev_flash
appearing) while the process runs, close the lingering GUI on success, keep the 10-minute
kill+fail for a truly wedged installer. This plan applies the same shape to game updates.

## Result signal (and why version alone is not enough)

RPCS3 records the installed game-update version as `APP_VER` in
`<rpcs3Root>/dev_hdd0/game/<TITLEID>/PARAM.SFO` — RPCS3's own
`package_reader::check_target_app_version` reads exactly this file/key. `rpcs3Root` is the
same per-OS root `Ps3Firmware::devFlashRoot(binDir)` already resolves (dev_hdd0 sits beside
dev_flash under RPCS3's config root; binDir on Windows portable installs).

**Caveat discovered in RPCS3 source** (`Crypto/unpkg.cpp`, `extract_worker`): pkg entries
are extracted IN PLACE, in entry order — `PARAM.SFO` can land early, long before the rest
of the update's files. So "APP_VER reached the target" is necessary but NOT sufficient;
killing RPCS3 at that moment could destroy a mid-flight install and record it as applied.
The success predicate is therefore: **version at target AND the game dir has gone quiet**
(nothing under `dev_hdd0/game/<TITLEID>` changing for ~3s).

**Quiescence cannot be an mtime scan.** On Windows/NTFS a path-based `QFileInfo::lastModified()`
reads the *directory entry*, which is updated LAZILY — typically only when the writer closes or
flushes the handle. A single multi-gigabyte payload file being written right now, with no other
file activity, therefore reads as perfectly quiet, and we would kill RPCS3 mid-write and record
the truncated update as applied. Quiescence is instead a **fingerprint of the whole tree** whose
file sizes are read from an OPEN handle (`QFile::size()` on a file we opened), which is real-time;
mtimes are folded in as extra churn signal only. A file that cannot be opened at all (exclusive
writer lock) means "definitely busy", not "unchanged".

## Global constraints

- The Installer seam stays a `std::function` on `Ps3UpdateInstaller`; pure/probe-testable
  logic lives in `src/core/ps3` units exercised by `probe_ps3update` (no display, no
  network, no process spawns in the probe).
- Sliced waits: the lambda must keep polling `QThread::currentThread()->isInterruptionRequested()`
  (app-quit teardown, commit 7adf4a0) — never a long blocking wait.
- The 10-minute `QDeadlineTimer` kill+fail stays, covering a truly wedged installer.
- A process that exits on its own is honored: success iff the version actually reached the
  target (exit code 0 with no version on disk is a failure, mirroring the fw lambda).
- Every internal failure falls through — the game always boots.
- No AI attribution in commits. Conventional `fix:` prefix.

## Task 1 — core/ps3 units + probe coverage

**`Ps3Sfo`** (`native/src/core/ps3/Ps3Sfo.{h,cpp}`): generalize the existing parser.
- Add `std::optional<QString> stringValue(const QByteArray& sfo, const QByteArray& key);`
  — exactly the current `titleIdFromSfo` loop with `key` parameterized (same bounds guards,
  same null-termination handling, `QString::fromLatin1` — SFO string values here are ASCII).
- Reimplement `titleIdFromSfo` as `return stringValue(sfo, "TITLE_ID");`. Keep it declared.

**New unit `Ps3InstalledVersion`** (`native/src/core/ps3/Ps3InstalledVersion.{h,cpp}`):
```cpp
namespace Ps3InstalledVersion {
// <rpcs3Root>/dev_hdd0/game/<titleId> — where RPCS3 lands a title's update data.
QString gameDir(const QString& rpcs3Root, const QString& titleId);
// APP_VER from <gameDir>/PARAM.SFO (VERSION as fallback when APP_VER is absent);
// nullopt when the file is missing/malformed or carries neither key.
std::optional<QString> installedVersion(const QString& gameDir);
// true iff installedVersion(gameDir) exists and is >= target (Ps3Version::less).
bool reachedTarget(const QString& gameDir, const QString& targetVersion);
// Stable fingerprint of every FILE under gameDir (recursive): equal across two scans
// of an unchanged tree, different after any write. nullopt = a file could not be
// opened = the writer holds it = definitely busy. Missing dir / no files = a valid,
// stable value ("nothing there yet" is not "busy"). Quiescence signal for an
// in-place pkg extraction; handle-based sizes, because NTFS mtimes lag (see above).
std::optional<QByteArray> dirFingerprint(const QString& gameDir);
}
```
Implementation: QDirIterator (Subdirectories, Files), sorted paths for stable ordering, each file
opened ReadOnly and hashed as (relative path, handle-based `QFile::size()`, path-based mtime).
Qt6::Core only. Uses Ps3Sfo + Ps3Version.

**`Ps3UpdateInstaller`** (`.h`/`.cpp`): extend the Installer contract with the context the
result check needs:
```cpp
using Installer = std::function<int(const QString& rpcs3Exe, const QString& pkgPath,
                                    const QString& titleId, const QString& version)>;
```
`installAll` passes `titleId` and `p.version` through. Update the header doc: the installer
returns 0 iff the title's installed version reached `version` (however it determines that);
non-zero aborts the chain.

Also add an optional 5th constructor parameter, defaulted to `{}` (absent = never skip):
```cpp
using AlreadyApplied = std::function<bool(const QString& titleId, const QString& version)>;
```
`installAll` consults it BEFORE downloading each package and `continue`s when it says yes — a
retry after a partial chain, or after a lost `ps3-updates.json`, must not re-pay hundreds of
megabytes only for the installer to discover the pkg is already on disk.

**CMake** (`native/CMakeLists.txt`): add `Ps3InstalledVersion.cpp`/`.h` to the app source
list (beside the other `src/core/ps3/` pairs, ~line 451) and `Ps3InstalledVersion.cpp` to
the `probe_ps3update` source list (~line 1115).

**Probe** (`native/tools/probe_ps3update.cpp`):
- Update all Installer stub lambdas to the 4-arg signature.
- `testSfo`: `stringValue` returns APP_VER from a `makeSfo` blob; missing key → nullopt;
  `titleIdFromSfo` still works (regression).
- `testInstaller`: the runner records `(titleId, version)` per call; assert the two-package
  chain threads `("BLUS31156","01.05")` then `("BLUS31156","01.11")` in order. Plus an
  `AlreadyApplied` case: a callback true for "01.05" only must skip that package's DOWNLOAD
  (the downloader records its calls) and its install, still install "01.11", and return true.
- New `testInstalledVersion`:
  - `gameDir("root","BLUS31156")` == `"root/dev_hdd0/game/BLUS31156"`.
  - Temp dir with `PARAM.SFO` = `makeSfo({{"APP_VER","01.05"},{"TITLE_ID","BLUS31156"}})`:
    `installedVersion` == "01.05"; `reachedTarget(dir,"01.05")` true; `(dir,"01.11")` false;
    `(dir,"01.04")` true (past target counts).
  - VERSION fallback: sfo with only `{"VERSION","01.02"}` → installedVersion == "01.02".
  - Missing dir / missing PARAM.SFO / garbage bytes → nullopt, reachedTarget false.
  - `dirFingerprint`: missing dir → has_value (a stable "nothing"); two scans of an
    unchanged dir → equal; appending bytes to a file → changed; adding a file, including
    in a nested subdir, → changed. (The nullopt/locked-file path is not portably
    simulable and is left to the comment.)
- Probe still prints `PS3UPDATE-OK`.

## Task 2 — EmulatorManager `--installpkg` lambda (result-based wait)

`native/src/core/EmulatorManager.cpp`, the Installer lambda passed to `Ps3UpdateInstaller`
(currently ~line 1500): capture `binDir`, take the 4-arg signature, and:

1. `const QString gameDir = Ps3InstalledVersion::gameDir(Ps3Firmware::devFlashRoot(binDir), titleId);`
2. **Pre-spawn short-circuit**: if `reachedTarget(gameDir, version)` already, return 0
   without spawning — the disk state IS the result (a lost/stale `ps3-updates.json` re-runs
   an already-applied update; spawning would risk killing a reinstall mid-write).
   `installAll` then still calls `markInstalled`, healing the state file.
3. Spawn `--installpkg`, `waitForStarted(30000)` else -1.
4. Sliced loop against `QDeadlineTimer deadline(600000)`:
   - `proc.waitForFinished(500)` → it exited on its own:
     `return reachedTarget(...) ? 0 : (proc.exitCode() == 0 ? -1 : proc.exitCode());`
   - interruption requested or deadline expired → `kill(); waitForFinished(5000); return -1;`
   - `reachedTarget(gameDir, version)` AND `dirFingerprint(gameDir)` valid and unchanged for
     >= 3s (carry the last fingerprint plus a `QElapsedTimer` across iterations; nullopt or a
     changed fingerprint restarts the window) → `proc.waitForFinished(2000)` (settle: let the
     installer close handles; it won't exit), `kill(); waitForFinished(5000); return 0;`
5. Comments must carry the three constraints the code can't show: (a) RPCS3 stays open as the
   GUI after installing, so waiting for exit kills successful installs (hardware 2026-08-19,
   fw twin); (b) pkg entries extract in place in entry order, so PARAM.SFO can land early —
   version-at-target alone must not trigger the kill; (c) NTFS updates directory-entry mtimes
   lazily, so a path-mtime quiescence check is a lie during a long single-file write — the
   fingerprint's handle-based sizes are the real-time signal. Match the fw lambda's comment
   style directly above.
6. Pass the `AlreadyApplied` callback when constructing `Ps3UpdateInstaller`:
   `[binDir](titleId, version) { return reachedTarget(gameDir(devFlashRoot(binDir), titleId), version); }`.
   The lambda's own pre-spawn short-circuit stays as a cheap second line of defense — the
   Installer contract promises 0-iff-reached however the installer is constructed.

Includes: `Ps3InstalledVersion.h` (QDeadlineTimer already included by f514390).

Idempotency semantics: coordinator `markInstalled(titleId, latest)` fires only when
`installAll` returns true, which now means "every package's version verifiably reached disk
(or already had)". A quit-interrupted or deadline-killed install returns -1 → no record →
clean retry next launch. On that retry the `AlreadyApplied` callback skips every
already-landed package BEFORE its download, so the retry costs nothing but a PARAM.SFO read
— the lambda's pre-spawn short-circuit alone would still have re-paid the whole download
before discovering the same thing.

## Task 3 — build + full headless gate

Per CONTRIBUTING: configure if needed (session-worktree CMake flags: Qt prefix + the MPV_* pair),
`cmake --build build --config Release` for the app + all probes (never target-less if
CONTRIBUTING forbids it — follow its stated recipe), then
`BUILD_DIR=build bash native/tools/run-headless-probes.sh` and require the literal
`ALL HEADLESS PROBES PASSED`.

## After the tasks

Fable final review → merge to main + push origin → deploy Release exe to
C:\EverythingBox-app (standing autonomy to close the running app; Release config only).
