# PS3 Update Worker Quit Teardown Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Stop the RPCS3 PS3-update worker thread cleanly at app quit (interrupt + bounded join, child installer killed), and guarantee a killed `--installfw` can never leave `Ps3Firmware::installed()` half-true.

**Architecture:** Two independent changes. (1) `Ps3Firmware::maybeInstall` gains a defensive cleanup: after an installer run that *failed* (non-zero exit — which includes "we killed it"), any `version.txt` that appeared during that failed run is removed, so `installed()` reads false and the next launch retries (behind the existing 1-hour backoff marker). This is needed regardless of RPCS3's extraction order: a kill can land mid-write of `version.txt` itself, leaving it non-empty. (2) `EmulatorManager::runPs3UpdateThenLaunch` makes the worker cooperatively interruptible (`QThread::isInterruptionRequested()` polled by the network waits and the bounded process waits, which kill the child RPCS3 on interruption) and connects `QCoreApplication::aboutToQuit` to `requestInterruption()` + a bounded `wait()`, with the worker itself as the connection context so the handler auto-disconnects when the worker self-deletes and N live workers each get their own handler.

**Tech Stack:** Qt 6.8 (QThread interruption, QDeadlineTimer, QTimer, QProcess), existing probe gate (`probe_ps3firmware` + `native/tools/mutate.py` matrix `native/tools/mutate-ps3fwroot.json`).

## Global Constraints

- No AI attribution anywhere: no `Co-Authored-By`, no "Generated with" lines, in commits, PR bodies, comments.
- Conventional commit prefixes (`fix:` here). No `Fixes #NNN` trailer — this work has no GitHub issue (it is a session chip).
- Every new probe assertion must be proven by a mutant in the matrix, run through `native/tools/mutate.py` (the one driver). CRLF gotcha: the driver's find/replace strings must match the file's actual line endings.
- Probe registration rule (three places) does **not** apply here — no new probe target, only new checks in the existing `probe_ps3firmware`.
- Build recipe for this fresh worktree (no `build/` exists yet — run once before Task 1's build step):

```bash
cd "/c/Users/cubma/Project Goliath/.claude/worktrees/friendly-spence-c871e3"
git submodule update --init external/RetroPark
cmake -S native -B build -G "Visual Studio 18 2026" -A x64 -DEVERYTHINGBOX_BUILD_APP=ON \
  -DCMAKE_PREFIX_PATH=C:/Qt/6.8.3/msvc2022_64 \
  -DMPV_INCLUDE_DIR=C:/mpv-dev/include -DMPV_LIBRARY=C:/mpv-dev/libmpv.lib
```

  The configure log must say `data-dir isolation applied to N probe target(s)` with N ≈ 52, **not** 2 (2 means `EVERYTHINGBOX_BUILD_APP` ended up OFF).
- Run probes with: `PATH="/c/Qt/6.8.3/msvc2022_64/bin:$PATH" QT_QPA_PLATFORM=offscreen QT_PLUGIN_PATH="C:/Qt/6.8.3/msvc2022_64/plugins"`. Exit 127 / rc124 hang = missing DLL/plugin env, not a test failure.
- Build and test **synchronously in your shell** (`cmake --build ... --parallel`, then run the probe). Do not hand control back "waiting for a build monitor" — wait for the command to finish and paste its tail.

---

### Task 1: `Ps3Firmware::maybeInstall` — a failed installer never leaves `installed()` true

**Files:**
- Modify: `native/src/core/ps3/Ps3Firmware.cpp` (maybeInstall tail, ~lines 105–115)
- Modify: `native/src/core/ps3/Ps3Firmware.h` (doc comment on `maybeInstall` only)
- Test: `native/tools/probe_ps3firmware.cpp` (new test function + call in `main`)
- Modify: `native/tools/mutate-ps3fwroot.json` (two new mutants)

**Interfaces:**
- Consumes: existing `Ps3Firmware::maybeInstall` seams (`FeedFetcher`, `Downloader`, `Installer`, `Progress`) and probe helpers `seedVersionTxt(root, bytes)`, `markerPath(tmpDir)`, `kRealFeed` — all already in `probe_ps3firmware.cpp`.
- Produces: no signature change. New guaranteed post-condition consumed by Task 2's reasoning: after `maybeInstall` returns false because the installer ran and exited non-zero, `installed(fwRoot)` is false.

- [ ] **Step 1: Write the failing test**

In `native/tools/probe_ps3firmware.cpp`, add after `testInstalled()` (adjust placement to wherever the other `test*` functions sit; keep the file's style):

```cpp
// A killed --installfw (bounded-run timeout, or app-quit interruption in EmulatorManager) surfaces here
// as a non-zero installer exit — but the kill may land after RPCS3 already wrote version.txt (or mid-write,
// leaving it non-empty). installed() would then read true forever over a broken dev_flash, and the entry
// check would skip every future repair. maybeInstall must scrub the half-written version.txt so the next
// launch (after the backoff) retries.
static void testKilledInstallerLeavesUninstalled()
{
    QTemporaryDir dir; CHECK(dir.isValid());
    const QString root = dir.path() + QStringLiteral("/root");
    const QString tmp  = dir.path() + QStringLiteral("/tmp");
    const bool r = Ps3Firmware::maybeInstall(root, QStringLiteral("rpcs3.exe"), tmp,
        [] { return std::optional<QByteArray>(QByteArray(kRealFeed)); },
        [](const QString&, const QString& dest) {
            QFile f(dest); CHECK(f.open(QIODevice::WriteOnly)); f.write("pup");
            return true;
        },
        [&root](const QString&, const QString&) {
            seedVersionTxt(root, QByteArray("04.9200")); // the kill landed after version.txt was extracted
            return -1;                                   // …and the process died non-zero
        },
        nullptr);
    CHECK(!r);                                    // never reported as success
    CHECK(!Ps3Firmware::installed(root));         // the half-install was scrubbed: next attempt retries
    CHECK(QFile::exists(markerPath(tmp)));        // and the hourly backoff still applies
}
```

Add `testKilledInstallerLeavesUninstalled();` to `main()` next to the other calls.

- [ ] **Step 2: Build the probe and verify the new test fails**

```bash
cd "/c/Users/cubma/Project Goliath/.claude/worktrees/friendly-spence-c871e3"
cmake --build build --config Release --target probe_ps3firmware --parallel
PATH="/c/Qt/6.8.3/msvc2022_64/bin:$PATH" QT_QPA_PLATFORM=offscreen ./build/Release/probe_ps3firmware.exe
```

Expected: `CHECK failed: !Ps3Firmware::installed(root)` and **no** `PS3FIRMWARE-OK` sentinel (exit non-zero). The other two new CHECKs pass already (non-zero exit → `ok` false → marker + false return).

- [ ] **Step 3: Implement the cleanup in `maybeInstall`**

In `native/src/core/ps3/Ps3Firmware.cpp`, replace the tail of `maybeInstall` (currently):

```cpp
    QDir().mkpath(tmpDir);
    const QString pup = QDir(tmpDir).filePath(QStringLiteral("PS3UPDAT.PUP"));
    bool ok = download && download(info->url, pup);
    if (ok) ok = install && install(rpcs3Exe, pup) == 0;
    if (QFile::exists(pup)) QFile::remove(pup); // the PUP is only a means to dev_flash — never leave ~230MB behind, pass or fail

    const bool done = ok && installed(fwRoot);
```

with:

```cpp
    QDir().mkpath(tmpDir);
    const QString pup = QDir(tmpDir).filePath(QStringLiteral("PS3UPDAT.PUP"));
    const bool downloaded  = download && download(info->url, pup);
    const bool ranInstaller = downloaded && static_cast<bool>(install);
    const bool installedOk  = ranInstaller && install(rpcs3Exe, pup) == 0;
    if (QFile::exists(pup)) QFile::remove(pup); // the PUP is only a means to dev_flash — never leave ~230MB behind, pass or fail

    // An installer that ran and failed (which includes "the caller killed it": bounded-run timeout, or
    // app-quit interruption) may still have written version.txt — the kill can even land mid-write and
    // leave it non-empty. installed() would then read true over a broken dev_flash and the entry check
    // would skip every future repair. Firmware was absent when this function was entered, so a
    // version.txt present now was written by this failed attempt and is safe to scrub.
    if (ranInstaller && !installedOk && installed(fwRoot))
        QFile::remove(fwRoot + QStringLiteral("/dev_flash/vsh/etc/version.txt"));

    const bool done = installedOk && installed(fwRoot);
```

(The `if (done) QFile::remove(marker); if (!done) noteFailure(...); return done;` lines below stay unchanged.)

In `native/src/core/ps3/Ps3Firmware.h`, extend the `maybeInstall` doc comment's failure sentence — after "so a persistently failing install cannot re-download ~230MB before every launch." add:

```
// A failed installer run additionally scrubs any dev_flash version.txt it left behind, so a killed
// --installfw (timeout, or app-quit interruption) can never make installed() read half-true.
```

- [ ] **Step 4: Rebuild and verify the probe passes**

Same commands as Step 2. Expected: `PS3FIRMWARE-OK`, exit 0. All pre-existing checks must still pass — in particular the success-path test (installer succeeds → `installed()` true) proves the scrub does not fire on success.

- [ ] **Step 5: Add mutants proving both sides of the scrub, run the mutation driver**

In `native/tools/mutate-ps3fwroot.json`, append to `"mutants"` (mind the file's line endings — check with `file native/tools/mutate-ps3fwroot.json` / a hexdump of one line; if the source file is CRLF, multi-line `find` strings must encode `\r\n`, or use single-line finds as below):

```json
    { "name": "halftrue-scrub-deleted",
      "file": "native/src/core/ps3/Ps3Firmware.cpp",
      "find": "        QFile::remove(fwRoot + QStringLiteral(\"/dev_flash/vsh/etc/version.txt\"));",
      "replace": "        { /* mutated: scrub skipped */ }",
      "count": 1, "expect": "killed" },

    { "name": "halftrue-scrub-on-success",
      "file": "native/src/core/ps3/Ps3Firmware.cpp",
      "find": "    if (ranInstaller && !installedOk && installed(fwRoot))",
      "replace": "    if (ranInstaller && installed(fwRoot))",
      "count": 1, "expect": "killed" }
```

The first is killed by Step 1's `!installed(root)` check; the second scrubs on *success* too and is killed by the existing success-path test asserting `installed()` stays true.

Run the driver from the worktree root:

```bash
python native/tools/mutate.py native/tools/mutate-ps3fwroot.json
```

Expected: every mutant reports `killed` (previously 4, now 6), and the final line confirms the tree was restored (run `git status --short` after — only your intended edits may show).

- [ ] **Step 6: Commit**

```bash
git add native/src/core/ps3/Ps3Firmware.cpp native/src/core/ps3/Ps3Firmware.h native/tools/probe_ps3firmware.cpp native/tools/mutate-ps3fwroot.json
git commit -m "fix: scrub the half-written dev_flash version.txt a failed PS3 firmware install leaves behind

A killed --installfw (bounded-run timeout, or the app quitting mid-install)
exits non-zero but may already have written dev_flash/vsh/etc/version.txt --
or died mid-write, leaving it non-empty. installed() then reads true over a
broken dev_flash and the entry check skips every future repair, permanently.
maybeInstall only runs when firmware was absent at entry, so any version.txt
present after a failed installer run belongs to that run and is scrubbed,
making the next launch (after the hourly backoff) retry cleanly."
```

---

### Task 2: `EmulatorManager` — interrupt + join the PS3 update worker at app quit

**Files:**
- Modify: `native/src/core/EmulatorManager.cpp` (`fetchSonyTextFeed` ~1307, `downloadPs3Pkg` ~1351, `runPs3UpdateThenLaunch` ~1395–1488)
- Modify: `native/src/core/EmulatorManager.h` (the `launchCtx_` comment, ~lines 95–101)

**Interfaces:**
- Consumes: Task 1's post-condition (a killed `--installfw` leaves `installed()` false), plus the existing guards: `note`'s QPointer pair, the `launchCtx_`-bound continuation.
- Produces: nothing new for other code. Behavior contract: on `QCoreApplication::aboutToQuit`, every live PS3 update worker is interruption-requested and joined for up to 8 s; the in-flight network reply aborts within ~500 ms; an in-flight `--installfw`/`--installpkg` child is killed and reaped (≤ ~5.6 s worst case). A superseded-but-running worker (its launch context retired) gets the same treatment because each worker carries its own `aboutToQuit` connection.

There is no headless probe for this file (GUI-coupled unit; the seams under test all live in `Ps3Firmware`/`Ps3Update*`, already gated). Verification is compile + the full probe suite staying green (Task 3) plus the code-reasoning below — keep the comments precise, they are the spec.

- [ ] **Step 1: Make the two network helpers interruption-aware**

In `native/src/core/EmulatorManager.cpp`, `fetchSonyTextFeed`: after the existing hard-watchdog `QTimer::singleShot(20000, ...)` line and before `loop.exec();`, insert:

```cpp
    // App-quit teardown: the RPCS3 update worker is interruption-requested on aboutToQuit. This wait
    // runs on that worker thread, so poll the flag and abort the reply — finished fires, the loop
    // quits, and the aborted reply reads as an error -> nullopt, exactly like any failed fetch.
    QTimer interruptPoll;
    interruptPoll.setInterval(500);
    QObject::connect(&interruptPoll, &QTimer::timeout, reply, [reply] {
        if (QThread::currentThread()->isInterruptionRequested() && reply->isRunning()) reply->abort();
    });
    interruptPoll.start();
```

Apply the identical block in `downloadPs3Pkg`, after its `QTimer::singleShot(900000, ...)` line and before its `loop.exec();` (same code, same comment condensed to one line: `// App-quit teardown: see fetchSonyTextFeed — an aborted reply reads as a failed download, and the partial file is removed below.`).

Add `#include <QThread>` to the include block at the top of `EmulatorManager.cpp` if it is not already there (check; `QTimer` is already in use).

- [ ] **Step 2: Make both bounded installer runs interruption-aware**

Still in `EmulatorManager.cpp`, inside `runPs3UpdateThenLaunch`, replace the `--installfw` runner body:

```cpp
                QProcess proc;
                proc.start(exe, { QStringLiteral("--installfw"), pup });
                if (!proc.waitForStarted(30000)) return -1;
                if (!proc.waitForFinished(600000)) { proc.kill(); proc.waitForFinished(5000); return -1; }
                return proc.exitCode();
```

with:

```cpp
                QProcess proc;
                proc.start(exe, { QStringLiteral("--installfw"), pup });
                if (!proc.waitForStarted(30000)) return -1;
                // Wait in slices so an app-quit interruption request kills the installer within ~500ms
                // instead of blocking Qt teardown for up to 10 min; the deadline keeps the original
                // wedge protection. Either way a killed run returns non-zero, and Ps3Firmware scrubs
                // the half-written version.txt so installed() never reads half-true.
                QDeadlineTimer deadline(600000);
                while (!proc.waitForFinished(500))
                {
                    if (!QThread::currentThread()->isInterruptionRequested() && !deadline.hasExpired())
                        continue;
                    proc.kill(); proc.waitForFinished(5000); return -1;
                }
                return proc.exitCode();
```

Apply the same transformation to the `--installpkg` runner a few lines below (identical body, `--installpkg` argument; condense the comment to `// Sliced wait: see the --installfw runner above — interruption or the 10-min deadline kills it.`). Update both runners' existing "Bounded run instead of QProcess::execute…" lead comments only if they now contradict the code (they don't — keep them).

Add `#include <QDeadlineTimer>` to the include block if absent.

- [ ] **Step 3: Early-out the worker between stages and join it on aboutToQuit**

(a) First line of the worker lambda (before the `note` definition):

```cpp
        if (QThread::currentThread()->isInterruptionRequested()) return; // app already quitting
```

and replace `if (!gameUpdates) return;` with:

```cpp
        if (!gameUpdates || QThread::currentThread()->isInterruptionRequested()) return;
```

(b) After the existing `connect(worker, &QThread::finished, worker, &QObject::deleteLater);` and before the `launchCtx_` continuation connect, insert:

```cpp
    // App-quit teardown: without this, the worker runs on through Qt teardown (its local event loops
    // and QNetworkAccessManager outlive the Qt globals — deleteLater is never delivered once the loop
    // stops) and can leave a half-run --installfw behind. Request interruption — the network waits and
    // the sliced process waits above poll it — and join for a bounded interval so quit is never held
    // hostage by a slow kill (worst case ~5.6s: one 500ms slice + the 5s reap). The worker is the
    // connection context, so a finished-and-deleted worker drops its handler automatically and every
    // live worker (including one whose launch was superseded) gets its own.
    connect(qApp, &QCoreApplication::aboutToQuit, worker, [worker] {
        worker->requestInterruption();
        worker->wait(8000);
    });
```

`qApp` requires `#include <QCoreApplication>` — check the include block (likely present transitively; add explicitly if not). Note the receiver (`worker`) lives on the UI thread, so the handler runs directly on the UI thread during `aboutToQuit`, before Qt teardown begins.

(c) In `native/src/core/EmulatorManager.h`, the `launchCtx_` comment currently ends: `The worker thread itself is never cancelled (it runs to completion; its installs are idempotent) — only its continuation is gated, which is all supersession needs.` Replace that final sentence with:

```
    // For supersession only the continuation is gated (the worker runs to completion; its installs are
    // idempotent). App quit is the exception: aboutToQuit interruption-requests and joins every live
    // worker (bounded), killing an in-flight installer child — see runPs3UpdateThenLaunch.
```

- [ ] **Step 4: Build the app target and the touched probes**

```bash
cmake --build build --config Release --target EverythingBox probe_ps3firmware probe_ps3update --parallel 2>&1 | tail -5
```

Expected: exit 0, no warnings introduced in `EmulatorManager.cpp`. Then re-run `probe_ps3firmware.exe` (env as in Task 1 Step 2) — still `PS3FIRMWARE-OK`.

- [ ] **Step 5: Commit**

```bash
git add native/src/core/EmulatorManager.cpp native/src/core/EmulatorManager.h
git commit -m "fix: interrupt and join the RPCS3 update worker at app quit

The PS3 firmware/update worker thread was never stopped: if the app quit
during the PUP/PKG download or a child --installfw/--installpkg run, the
thread kept executing through Qt teardown (its local event loops and
QNetworkAccessManager outlive the Qt globals; its deleteLater is never
delivered), risking an exit-time crash and a half-run install.

aboutToQuit now requests interruption on every live worker and joins it for
a bounded interval. The network waits poll the flag and abort their reply
(partial downloads were already removed on failure), and the installer
waits run in slices and kill the child, whose non-zero exit makes
Ps3Firmware scrub any half-written version.txt. Each worker carries its own
auto-disconnecting handler, so superseded-but-running workers are covered."
```

---

### Task 3: Full gate, review, merge

(Orchestrator-level; listed for completeness.)

- [ ] Build the full CI probe list (the `--target` list in the "Build probes" step of `.github/workflows/ci.yml`, plus `probe_mpvpreview`), then run `BUILD_DIR=build bash native/tools/run-headless-probes.sh` with the full env recipe. Expected: suite green end-to-end (the runner prints its final summary; no probe "not built / not rebuilt").
- [ ] `bash -n native/tools/run-headless-probes.sh` (untouched here, but cheap and mandated after merges).
- [ ] Code review (Fable), address findings.
- [ ] Merge to `main`, push `origin main`; rebuild-all-and-grep-errors on the merged main tree.
