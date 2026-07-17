# Perf Track Implementation Plan (Phase 2, Plan 1)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the PerfTrace span harness, instrument the four pain areas, capture a committed baseline, fix hotspots in measured order with before/after evidence, and lock component budgets into a CI probe.

**Architecture:** A tiny always-compiled tracing module (`MMV_PERF=1` gated) + span sites at the seams phase 1 exposed; a Python baseline runner drives the standard route through the existing uitest pipe and ranks spans; fixes are data-driven tasks instantiated by the controller from the ranking. Spec: `docs/superpowers/specs/2026-07-17-perf-track-design.md`.

**Tech Stack:** Qt 6 C++ (QElapsedTimer/QMutex), Python 3 (reuses `native/tools/uitest.py`), headless probe pattern.

## Global Constraints

- Behavior preservation: perf fixes must not change visible behavior or ordering; probe_nav + browse/playback probes green on every commit.
- Measure up to — never inside — mpv, libretro cores, Qt internals; network latency is overlap/cache territory only.
- HANDS OFF until their sessions land on main: the FavoritesStore write path and the OSK focus handoff (two independent bug-fix sessions own them). Before each task, `git pull` main is NOT needed (work on the branch), but if a merge from main happens, re-check for conflicts in those areas.
- Trace lines must never leak secrets: detail fields carry file NAMES and counts only (reuse the `logSafeUrl` idiom for anything URL-shaped).
- Build: `cmake --build build --config Release [--target <t>]` from repo root. Probes: prepend `/c/Qt/6.8.3/msvc2022_64/bin` (and `/c/mpv-dev` for the full runner) to PATH, `QT_QPA_PLATFORM=offscreen`. Full runner: `BUILD_DIR=build bash native/tools/run-headless-probes.sh`.
- Live: deploy Release exe over the existing exe in `C:\MyMediaVault-app` (keep name), launch detached with the needed env (`MMV_UITEST=1`, plus `MMV_PERF=1` for trace runs), drive only via `python native/tools/uitest.py`, never SendKeys/focus, kill after, protect user data (real playlist "Weekend Picks" exists).
- CI lesson from plan 2: a new probe must be added to BOTH `native/tools/run-headless-probes.sh` AND the build-target list at `.github/workflows/ci.yml:52`, or its gate silently never runs.

---

### Task 1: PerfTrace module + probe_perf (trace-behavior asserts)

**Files:**
- Create: `native/src/core/PerfTrace.h`, `native/src/core/PerfTrace.cpp`
- Create: `native/tools/probe_perf.cpp`
- Modify: `native/CMakeLists.txt` (app source list + probe_perf target + nothing else)
- Modify: `native/tools/run-headless-probes.sh` (register `probe_perf PERF-OK` in the new-probes loop's list)
- Modify: `.github/workflows/ci.yml:52` (append `probe_perf` to the build-target list)

**Interfaces (later tasks rely on these exact names):**
- `namespace PerfTrace`: `bool enabled();` · `void write(const QString& span, qint64 ms, const QString& detail = QString());` · `void begin(const QString& span);` · `void end(const QString& span, const QString& detail = QString());` · `class Scope { public: explicit Scope(const QString& name); void setDetail(const QString& d); ~Scope(); };` · `QString logPath();` · `void forceEnableForTest(const QString& logFile);`
- Macro: `PERF_SPAN("name")` (RAII scope for one function).
- Semantics (binding): `begin` on an already-open span name overwrites it (restarts the clock); `end` without a matching `begin` is a silent no-op (nav keys that never produce a measurable end must not corrupt the log); thread-safe via one QMutex; when `!enabled()` every call is a single cached-bool branch and no allocation.

- [ ] **Step 1: Write the failing probe**

`native/tools/probe_perf.cpp`:

```cpp
// Headless test for the PerfTrace span harness (phase-2 perf track): gating, line format,
// begin/end orphan semantics, and the disabled-path overhead budget. Prints PERF-OK.
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QThread>
#include "../src/core/PerfTrace.h"

static int fails = 0;
#define CHECK(cond, name) do { if (cond) printf("PASS %s\n", name); \
    else { printf("FAIL %s\n", name); ++fails; } } while (0)

static QStringList lines(const QString& p)
{
    QFile f(p);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
    return QString::fromUtf8(f.readAll()).split('\n', Qt::SkipEmptyParts);
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir tmp;
    const QString log = tmp.filePath("perf.log");

    CHECK(!PerfTrace::enabled(), "disabled by default (no MMV_PERF)");
    { PERF_SPAN("dead.span"); }              // must be a no-op while disabled
    PerfTrace::begin("dead.b"); PerfTrace::end("dead.b");
    CHECK(lines(log).isEmpty(), "disabled emits nothing to the test log");
    // (Do NOT assert the app's real perf_trace.log is absent — a prior MMV_PERF run may have left one.)

    // Disabled-path overhead budget: 1M scoped spans well under 200ms (it's one branch each).
    { QElapsedTimer t; t.start();
      for (int i = 0; i < 1000000; ++i) { PERF_SPAN("dead.hot"); }
      CHECK(t.elapsed() < 200, "disabled span overhead under budget"); }

    PerfTrace::forceEnableForTest(log);
    CHECK(PerfTrace::enabled(), "forceEnableForTest enables");

    { PERF_SPAN("unit.scope"); QThread::msleep(12); }
    PerfTrace::begin("unit.be");
    QThread::msleep(5);
    PerfTrace::end("unit.be", QStringLiteral("n=3"));
    PerfTrace::end("unit.orphan");           // no begin -> silent no-op
    PerfTrace::begin("unit.restart");
    QThread::msleep(30);
    PerfTrace::begin("unit.restart");        // overwrite restarts the clock
    QThread::msleep(5);
    PerfTrace::end("unit.restart");

    const QStringList out = lines(log);
    CHECK(out.size() == 3, "exactly the three real spans logged");
    // Format: ISO-ts | span | ms | detail(optional)
    const QRegularExpression re(QStringLiteral(
        "^\\d{4}-\\d{2}-\\d{2}T[0-9:.]+ \\| [a-z.]+ \\| \\d+(?: \\| .*)?$"));
    bool fmt = true;
    for (const QString& l : out) fmt = fmt && re.match(l).hasMatch();
    CHECK(fmt, "line format ISO-ts | span | ms | detail");
    CHECK(out[0].contains("unit.scope"), "scope span logged");
    CHECK(out[1].contains("unit.be") && out[1].contains("n=3"), "begin/end span carries detail");
    bool restartOk = false;
    { const QStringList parts = out[2].split(QStringLiteral(" | "));
      restartOk = parts.size() >= 3 && parts[1] == QStringLiteral("unit.restart")
                  && parts[2].toLongLong() < 25; }   // 5ms run, NOT 35ms — overwrite restarted it
    CHECK(restartOk, "begin-overwrite restarts the clock");

    if (fails == 0) printf("PERF-OK\n");
    return fails == 0 ? 0 : 1;
}
```

CMake (after the probe_browse block):

```cmake
    # Headless test for the PerfTrace span harness (gating, format, overhead budget).
    add_executable(probe_perf tools/probe_perf.cpp
        src/core/PerfTrace.cpp src/core/PerfTrace.h)
    target_include_directories(probe_perf PRIVATE src src/core)
    target_link_libraries(probe_perf PRIVATE Qt6::Core)
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build build --config Release --target probe_perf`
Expected: FAIL — `PerfTrace.h: No such file or directory`

- [ ] **Step 3: Implement PerfTrace**

`native/src/core/PerfTrace.h`:

```cpp
#pragma once
#include <QString>
#include <QElapsedTimer>

// Lightweight span tracing for the phase-2 perf track. Enabled by MMV_PERF=1 (or
// forceEnableForTest in probes); when disabled every call is one cached-bool branch.
// Lines land in <dataDir>/perf_trace.log as:  ISO-ts | span.name | duration_ms | detail
// Semantics: begin() on an open span RESTARTS it; end() without a begin is a no-op —
// so instrumentation sites never have to prove a matching pair fired.
namespace PerfTrace
{
    bool enabled();
    void write(const QString& span, qint64 ms, const QString& detail = QString());
    void begin(const QString& span);
    void end(const QString& span, const QString& detail = QString());
    QString logPath();
    void forceEnableForTest(const QString& logFile); // probes: enable + redirect output

    class Scope
    {
    public:
        explicit Scope(const QString& name);
        void setDetail(const QString& d);
        ~Scope();
    private:
        QString name_, detail_;
        QElapsedTimer t_;
        bool on_ = false;
    };
}

#define PERF_CAT2(a, b) a##b
#define PERF_CAT(a, b) PERF_CAT2(a, b)
#define PERF_SPAN(name) PerfTrace::Scope PERF_CAT(perfScope_, __LINE__)(QStringLiteral(name))
```

`native/src/core/PerfTrace.cpp`:

```cpp
#include "PerfTrace.h"
#include "AppPaths.h"
#include <QDateTime>
#include <QFile>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>

namespace
{
    bool g_forced = false;
    QString g_forcedPath;
    QMutex g_mutex;
    QHash<QString, QElapsedTimer> g_open; // begin()ed spans awaiting end()

    bool computeEnabled() { return g_forced || qEnvironmentVariableIntValue("MMV_PERF") == 1; }
}

namespace PerfTrace
{
    bool enabled()
    {
        static bool cached = computeEnabled();   // cached once; forceEnableForTest resets below
        return g_forced || cached;
    }

    QString logPath()
    {
        return g_forced ? g_forcedPath : AppPaths::dataDir() + QStringLiteral("/perf_trace.log");
    }

    void forceEnableForTest(const QString& logFile) { g_forced = true; g_forcedPath = logFile; }

    void write(const QString& span, qint64 ms, const QString& detail)
    {
        if (!enabled()) return;
        QMutexLocker lock(&g_mutex);
        QFile f(logPath());
        if (!f.open(QIODevice::Append | QIODevice::Text)) return;
        QString line = QDateTime::currentDateTime().toString(Qt::ISODateWithMs)
                     + QStringLiteral(" | ") + span + QStringLiteral(" | ") + QString::number(ms);
        if (!detail.isEmpty()) line += QStringLiteral(" | ") + detail;
        f.write((line + QStringLiteral("\n")).toUtf8());
    }

    void begin(const QString& span)
    {
        if (!enabled()) return;
        QMutexLocker lock(&g_mutex);
        g_open[span].start();                    // insert-or-overwrite restarts the clock
    }

    void end(const QString& span, const QString& detail)
    {
        if (!enabled()) return;
        qint64 ms = -1;
        {
            QMutexLocker lock(&g_mutex);
            const auto it = g_open.find(span);
            if (it == g_open.end()) return;      // orphan end: silent no-op
            ms = it->elapsed();
            g_open.erase(it);
        }
        write(span, ms, detail);
    }

    Scope::Scope(const QString& name) : name_(name), on_(enabled()) { if (on_) t_.start(); }
    void Scope::setDetail(const QString& d) { if (on_) detail_ = d; }
    Scope::~Scope() { if (on_) write(name_, t_.elapsed(), detail_); }
}
```

Add `src/core/PerfTrace.cpp src/core/PerfTrace.h` to the `qt_add_executable(mymediavault ...)` list.

- [ ] **Step 4: Run the probe**

Run: `cmake --build build --config Release --target probe_perf && PATH=/c/Qt/6.8.3/msvc2022_64/bin:$PATH QT_QPA_PLATFORM=offscreen ./build/Release/probe_perf.exe`
Expected: `PERF-OK`, exit 0. (If the format regex fights `Qt::ISODateWithMs` output, fix the IMPLEMENTATION to match the documented format, not the probe.)

- [ ] **Step 5: Register in the runner + CI**

`run-headless-probes.sh`: add `"probe_perf PERF-OK"` to the new-probes loop list. `.github/workflows/ci.yml:52`: append `probe_perf` to the `--target` list.

- [ ] **Step 6: Build the app target too, then commit**

Run: `cmake --build build --config Release --target mymediavault` — clean.

```bash
git add native/src/core/PerfTrace.h native/src/core/PerfTrace.cpp native/tools/probe_perf.cpp native/CMakeLists.txt native/tools/run-headless-probes.sh .github/workflows/ci.yml
git commit -m "perf: PerfTrace span harness (MMV_PERF=1), probe-tested, CI-gated"
```

---

### Task 2: Startup spans

**Files:**
- Modify: `native/src/main.cpp` (find it: `native/src/main.cpp`; anchor `int main(`)
- Modify: `native/src/ui/MainWindow.cpp` (constructor)

**Interfaces:**
- Consumes: Task 1's `PerfTrace::begin/end` + `PERF_SPAN`.
- Produces span names (Tasks 4/5 parse these exactly): `startup.total`, `startup.settings`, `startup.addons`, `startup.theme`, `startup.home`.

- [ ] **Step 1: Instrument**
  - `main.cpp`: first line of `main()` body: `PerfTrace::begin(QStringLiteral("startup.total"));` (include `core/PerfTrace.h`). Immediately after the main window `show()` call (grep `show()` in main.cpp): `QTimer::singleShot(0, [] { PerfTrace::end(QStringLiteral("startup.total")); });` — the zero-timer fires after the first paint pass.
  - MainWindow constructor: bracket the four phases with `PerfTrace::begin/end` pairs — `startup.settings` around the initial Settings/store reads at the top of the ctor; `startup.addons` around the `addons_` construction + `mgr` initial load block; `startup.theme` around the theme/BGM/theme-watcher setup block; `startup.home` around the home view construction + `showHomeScreen()` call. Anchor by reading the ctor top-to-bottom (it is long); each phase = the contiguous block whose comments name that concern. If a phase is not contiguous, span the dominant block and note the exclusion in a code comment on the begin line.
- [ ] **Step 2: Build + verify live**

Run: build app; deploy; launch detached with BOTH `MMV_PERF=1` and `MMV_UITEST=1`; wait for home via `uitest.py state`; kill the app. Then read `C:\MyMediaVault-app\perf_trace.log` (or the dataDir the app uses — grep `dataDir` resolution if the log isn't beside the exe): expect one line per span, `startup.total` present with plausible ms, phases summing to less than total.
- [ ] **Step 3: Run probe_nav + probe_perf (green), commit**

```bash
git add native/src/main.cpp native/src/ui/MainWindow.cpp
git commit -m "perf: startup spans (total + settings/addons/theme/home phases)"
```

---

### Task 3: Nav, open, and catalog spans

**Files:**
- Modify: `native/src/ui/MainWindow.cpp` (`sendNavKey`, mpv fileLoaded handler, `openGamePath` router)
- Modify: `native/src/launch/GameLauncher.cpp` (`open()`)
- Modify: `native/src/ui/HomeView.cpp` (`issueRequest`, `onCatalogReady`, `requestThemedMeta`, thumbnail queue, search connects)

**Interfaces:**
- Consumes: Task 1 API. Produces span names (exact): `nav.select`, `thumbs.page`, `open.video`, `open.audio`, `open.game`, `open.reader`, `catalog.load`, `search.first`, `search.drain`.
- Orphan-safe semantics from Task 1 are load-bearing here: several begins have no guaranteed end — that is by design, do NOT add pairing logic at call sites.

- [ ] **Step 1: Instrument** (each site is 1-3 lines; keep them comment-free except where the boundary needs explaining)
  - `nav.select`: in `MainWindow::sendNavKey`, for arrow keys only (`Qt::Key_Up/Down/Left/Right`): `PerfTrace::begin("nav.select")`. End in `HomeView::requestThemedMeta` right after the skeleton `themedMetaReady` emit: `PerfTrace::end(QStringLiteral("nav.select"))`. (Key with no selection change → orphaned begin, overwritten next press — fine.)
  - `thumbs.page`: in `HomeView::loadThumbnails` when the queue goes from empty to non-empty: `PerfTrace::begin("thumbs.page")`; in `pumpThumbnails` where a finished load decrements `thumbActive_`, when `thumbQueue_.isEmpty() && thumbActive_ == 0`: `PerfTrace::end(QStringLiteral("thumbs.page"), QStringLiteral("n=%1").arg(<count loaded this generation — track with a file-local or member int set in loadThumbnails>))`.
  - `open.video`/`open.audio`: `PerfTrace::begin("open.video")` at the top of `openVideoPath` and `playStream`; `begin("open.audio")` at the top of `openAudioPath` and `openAudioStream`. End BOTH names in the MpvWidget fileLoaded handler in MainWindow (grep `fileLoaded`): `PerfTrace::end(QStringLiteral("open.video")); PerfTrace::end(QStringLiteral("open.audio"));` — one is an orphan no-op, by design.
  - `open.game`: `begin` at the top of `MainWindow::openGamePath`; `end` in `GameLauncher::open()` immediately after the successful `retro_->openGame` branch (detail = ROM file name only), and also `end` right after `emu_->play(em, rom)` in `runEmulator` (external path measures to process handoff; detail = emulator display name). Add `#include "../core/PerfTrace.h"` to GameLauncher.cpp.
  - `open.reader`: `begin` at the top of `openDocumentPath`; `end` after the successful open branch for each reader type (grep the function's success paths).
  - `catalog.load`: in `HomeView::issueRequest`, before the request dispatch: `PerfTrace::begin("catalog.load")`; in `onCatalogReady` after `populate(...)` returns (the non-search path): `PerfTrace::end(QStringLiteral("catalog.load"), QStringLiteral("page=%1 n=%2").arg(pendingPage_).arg(cat.items.size()))`.
  - `search.first`/`search.drain`: in `HomeView::startSearch` (the Task-5-of-plan-2 rewrite) after `agg_->start(...)`: `begin("search.first"); begin("search.drain");`. In the `resultsAppended` connect, first batch only: `PerfTrace::end(QStringLiteral("search.first"), QStringLiteral("n=%1").arg(add.items.size()))`. In the `finished` connect: `PerfTrace::end(QStringLiteral("search.drain"), QStringLiteral("total=%1").arg(total))`.
- [ ] **Step 2: Build + verify live**: deploy; `MMV_PERF=1 MMV_UITEST=1` run; drive: scroll a console 10 rows, open a local video or audio file via Recents, open the local game, run one search; kill; assert every span name above appears in the log (except open.reader if no document is reachable — note it).
- [ ] **Step 3: Full probe suite green (behavior unchanged), commit**

```bash
git add native/src/ui/MainWindow.cpp native/src/ui/HomeView.cpp native/src/launch/GameLauncher.cpp
git commit -m "perf: nav/open/catalog/search spans across the phase-1 seams"
```

---

### Task 4: Baseline runner + committed baseline

**Files:**
- Create: `native/tools/perfbaseline.py`
- Create: `docs/superpowers/perf/2026-07-17-baseline.md` (generated + committed)

**Interfaces:**
- Consumes: the span names from Tasks 2-3 (exact list above) and `native/tools/uitest.py`'s `_send`/`state` helpers (import them: `sys.path.insert(0, os.path.dirname(__file__)); import uitest`).
- Produces: `perfbaseline.py run --exe <path> --log <perf_trace.log path> --out <md path>` and the committed baseline doc — Task 5 (the fix loop) reads its ranked table.

- [ ] **Step 1: Write the runner**

`native/tools/perfbaseline.py`:

```python
#!/usr/bin/env python3
"""Perf baseline runner (phase-2 perf track). Launches the deployed app with MMV_PERF=1 +
MMV_UITEST=1, drives the standard route via the uitest pipe, then parses perf_trace.log
into a ranked markdown table.

Usage: perfbaseline.py run --exe C:/MyMediaVault-app/<name>.exe --log <dataDir>/perf_trace.log --out docs/superpowers/perf/<date>-baseline.md

Standard route: cold start -> home settles -> Games -> first console -> 50-row scroll ->
open first game (local) -> wait 5s -> back out -> exit. Route steps that fail are recorded
as SKIPPED in the output, never silently dropped.
"""
import argparse, os, subprocess, sys, time
from collections import defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import uitest  # the existing pipe client


def wait_pipe(timeout=60):
    end = time.time() + timeout
    while time.time() < end:
        try:
            uitest.state(); return True
        except Exception:
            time.sleep(0.5)
    return False


def key(k, n=1, delay=0.15):
    for _ in range(n):
        uitest._send(f"key {k}"); time.sleep(delay)


def run_route(exe, log):
    if os.path.exists(log): os.remove(log)          # fresh trace per run
    env = dict(os.environ, MMV_PERF="1", MMV_UITEST="1")
    proc = subprocess.Popen([exe], env=env, cwd=os.path.dirname(exe))
    skipped = []
    try:
        if not wait_pipe(): raise SystemExit("app pipe never came up")
        time.sleep(3)                                # home settle (startup.total ends itself)
        key("down", 2); key("right", 3)              # reach Games category (route is themed-XMB)
        key("enter"); time.sleep(2)                  # first console
        key("down", 50, delay=0.08)                  # 50-row scroll (nav.select + thumbs spans)
        key("up", 50, delay=0.05)
        try:
            key("enter"); time.sleep(5)              # open first game if it's local
            key("escape"); time.sleep(1); key("enter"); time.sleep(2)  # esc menu -> back
        except Exception:
            skipped.append("open-game")
        key("escape", 3)                             # back to root
    finally:
        time.sleep(1)
        proc.kill()
    return skipped


def parse(log):
    spans = defaultdict(list)
    with open(log, encoding="utf-8", errors="replace") as f:
        for line in f:
            parts = [p.strip() for p in line.split("|")]
            if len(parts) >= 3:
                try: spans[parts[1]].append(int(parts[2]))
                except ValueError: pass
    return spans


def emit(spans, skipped, out):
    rows = sorted(((max(v), sum(v) // len(v), len(v), k) for k, v in spans.items()), reverse=True)
    with open(out, "w", encoding="utf-8") as f:
        f.write(f"# Perf baseline — {time.strftime('%Y-%m-%d %H:%M')}\n\n")
        f.write("Ranked by worst single occurrence on the standard route.\n\n")
        f.write("| span | worst ms | avg ms | samples |\n|---|---|---|---|\n")
        for worst, avg, n, name in rows:
            f.write(f"| {name} | {worst} | {avg} | {n} |\n")
        if skipped: f.write("\nSKIPPED route steps: " + ", ".join(skipped) + "\n")
    print(f"wrote {out} ({len(rows)} spans)")


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run")
    r.add_argument("--exe", required=True); r.add_argument("--log", required=True)
    r.add_argument("--out", required=True)
    a = ap.parse_args()
    skipped = run_route(a.exe, a.log)
    emit(parse(a.log), skipped, a.out)


if __name__ == "__main__":
    main()
```

Adjust the route's key sequence to the REAL themed-home layout by driving it manually with `uitest.py state` first — the sequence above is the shape, the state JSON is the truth. The route must end each run on the same screens or numbers aren't comparable; encode whatever fixups reality needs, in the script, with comments.

- [ ] **Step 2: Capture the baseline**

Run it 3 times (discard run 1 — OS cache warmup differs); commit run 3's output as `docs/superpowers/perf/2026-07-17-baseline.md`, appending a `## Run variance` note comparing runs 2 vs 3 (if any span differs >30% between them, mark it `noisy` in the doc — the fix loop treats noisy spans with suspicion).

- [ ] **Step 3: Commit**

```bash
git add native/tools/perfbaseline.py docs/superpowers/perf/2026-07-17-baseline.md
git commit -m "perf: baseline runner + committed baseline for the standard route"
```

---

### Task 5 (template, instantiated per hotspot by the controller): Fix hotspot `<span>`

**This task repeats.** The controller reads the baseline ranking and dispatches one instance per hotspot, top-down, until the spec's exit criteria are met. Each instance receives: the target span, its baseline numbers, and any suspect notes.

**Files:** whatever the hotspot needs — plus nothing else.

**Binding rules (from the spec):**
1. Diagnose first: reproduce the number locally (`perfbaseline.py run`), then localize the cost (add TEMPORARY child spans if needed — they may ship only if they'd be useful permanently; otherwise remove before commit).
2. Fix must be behavior-preserving (no visible ordering/content change). Known suspect classes: synchronous GUI-thread I/O, per-item `QSettings::sync`, redundant rebuilds, unbounded sequential loads. Off-limits: mpv/libretro/Qt internals, network speed itself, the two bug-fix sessions' areas.
3. Re-run the baseline route ≥2 times post-fix. The commit message MUST carry before/after: `perf: <what> — <span> <before>ms -> <after>ms on the standard route`.
4. No measured improvement (within run variance) → revert, record the attempt + verdict in `docs/superpowers/perf/2026-07-17-baseline.md` under `## Attempts`, and report honestly.
5. Full probe suite + a live smoke of the touched flow green before commit.
6. If the honest verdict is "at floor" (cost is inside mpv/core/Qt/network), write that verdict into the baseline doc under `## Verdicts` instead of forcing a fix.

---

### Task 6: probe_perf component budgets

**Files:**
- Modify: `native/tools/probe_perf.cpp` (append budget asserts)
- Modify: `native/CMakeLists.txt` (probe_perf sources gain the builder/search/resolver files + their deps, mirroring probe_browse's list)

**Interfaces:**
- Consumes: measured dev-box numbers from the Task 4/5 runs; `browse::` builders, `StreamResolver::parseM3u`, `SearchAggregator::acceptResult`, `PerfTrace` (already probed).

- [ ] **Step 1: Add budget asserts** — each budget = measured dev-box cost × ≥3 headroom, asserted with the measured value in a comment. Cover exactly:
  - `browse::recentsCatalog` over 5,000 synthetic RecentItems: budget from measurement (expect low tens of ms).
  - `browse::steamGamesCatalog` over 5,000 synthetic games with an injected pure poster fn.
  - `StreamResolver::parseM3u` on a generated 10,000-entry playlist string.
  - `SearchAggregator::acceptResult` over 10,000 items with 50% duplicates.
  Pattern per assert: build inputs, `QElapsedTimer`, run, `CHECK(t.elapsed() < BUDGET_MS, "...")` with `// measured Xms on 2026-07 dev box; 3x+ headroom` beside each budget constant.
- [ ] **Step 2: Build + run** — `PERF-OK` still the sentinel; all asserts PASS. Run the full runner once (everything green).
- [ ] **Step 3: Commit**

```bash
git add native/tools/probe_perf.cpp native/CMakeLists.txt
git commit -m "perf: component budgets in probe_perf (builders, m3u parse, search dedup)"
```

---

### Task 7: Close-out — exit-criteria audit + spec status

**Files:**
- Modify: `docs/superpowers/perf/2026-07-17-baseline.md` (final table + verdicts complete)
- Modify: `docs/superpowers/specs/2026-07-17-perf-track-design.md` (Status line)

- [ ] **Step 1:** Final `perfbaseline.py` run; append the final ranked table to the baseline doc as `## After` beside the original. Audit: every `startup.*`, `open.*`, `catalog.*` span has either a fix commit (named) or a verdict — fill any gap by escalating to the controller, not by writing a hollow verdict.
- [ ] **Step 2:** Full suite + live smoke (startup, scroll, open video/game, search) — green.
- [ ] **Step 3:** Spec Status → `Perf track complete: baseline + <N> measured fixes + <M> at-floor verdicts; budgets CI-gated. Polish track next.`
- [ ] **Step 4: Commit**

```bash
git add docs/superpowers/perf/2026-07-17-baseline.md docs/superpowers/specs/2026-07-17-perf-track-design.md
git commit -m "perf: close out the perf track — before/after tables, verdicts, spec status"
```
