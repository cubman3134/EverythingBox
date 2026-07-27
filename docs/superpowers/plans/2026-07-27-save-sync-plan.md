# Trustworthy Save Sync Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the whole-zip snapshot of emulator saves with per-file sync that has a real conflict rule, never silently loses a save, and stops one F2 press re-uploading the entire bundle.

**Architecture:** A pure `SaveSyncPlan` owns every "what should happen to this file" decision as a testable function over three views (local / remote / baseline) plus tombstones. A thin `SaveSync` drives it against Drive using `CloudSync`'s existing primitives, one Drive file per save. `saves/` and `states/` come out of the bundle and out of its fingerprint. Two data-loss bugs are fixed alongside: an unassigned `saveDir` and saves keyed to a cache hash.

**Tech Stack:** Qt 6.8.3 (Core/Network), C++17, MSVC 2022, Google Drive REST (`drive.file` scope). Headless probes as the test framework.

## Global Constraints

- **Build:** `export PATH="/c/Qt/6.8.3/msvc2022_64/bin:/c/mpv-dev:$PATH"`; build dir `build`; **always `--config Release`**. App target `everythingbox`.
- **Build ONLY named targets.** Never a target-less `cmake --build build` — it builds ~41 probes and stalls. Adding a source or probe needs exactly ONE reconfigure: `cmake -S native -B build -DEVERYTHINGBOX_BUILD_APP=ON` (no `-A`). Report BLOCKED past ~6 min with no progress.
- **When you add a `.cpp` to a CMake target, create the file in the SAME step.** CMake resolves source lists at configure time, so referencing a file no step has created yet fails the *reconfigure*, not the build — this has cost two tasks on this project already.
- **The suite script only RUNS pre-built exes.** After touching a shared source, rebuild every target that compiles it before trusting a green run. A green suite on stale binaries has happened here.
- **A new probe must be registered in THREE places** or it silently never runs: its `add_executable` block, the runner list at `native/tools/run-headless-probes.sh:119`, and the `--target` list in `.github/workflows/ci.yml`.
- **Exact constants:** `kSkewWindowMs = 5000`, `kUploadDebounceMs = 10000`, conflict name format `<base>.conflict-<deviceId>-<yyyyMMdd-HHmmss><ext>`, sweep extension allowlist `.srm .sav .brm .smpc .mcd .mcr .eep .fla .state`.
- **THE SAFETY RULE:** the first sync after this upgrade **never deletes anything**, in either direction. `firstRun == true` disables every `Delete*` outcome. Its failure mode is unrecoverable data loss, so it is asserted directly and mutation-tested.
- **THE PRESERVATION RULE:** on a conflict the losing copy is never destroyed. When the **remote** copy loses, its bytes exist only in the cloud and we are about to overwrite that name — the transport must **download the loser first**, write it as `.conflict-…`, and only then upload the winner.
- **Pre-commit hook** auto-bumps the patch version; let it. `EB_NO_VERSION_BUMP=1` skips it for docs-only commits.
- **Commit messages** end with `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`.

## File Structure

| File | Responsibility |
|---|---|
| `native/src/core/SaveSyncPlan.{h,cpp}` | **New.** Pure: `Entry`, `Act`, `Decision`, `plan()`, `conflictName()`, `isConflictArtifact()`. No network, no filesystem, no QSettings. |
| `native/src/core/SaveSync.{h,cpp}` | **New.** Transport: remote manifest, local baseline, per-file upload/download, torn-write guard, conflict ordering, debounce. |
| `native/src/core/SaveMeta.{h,cpp}` | **New.** The `saves-meta.json` sidecar — `{title, system, romPath, updatedAt}` per save file. |
| `native/src/core/CloudSync.cpp` | Remove `saves/`+`states/` from `buildBundle` (`:491-492`), from `applyBundle`, and from `stateHash` (`:563-565`). |
| `native/src/emu/RetroView.cpp` | Assign `core_.saveDir`; the one-time stray sweep; write sidecar entries on save. |
| `native/src/ui/MainWindow.cpp` | Own a `SaveSync`; startup pull, exit flush, the conflict notice. |
| `native/tools/probe_savesync.cpp` | **New.** All pure coverage. Sentinel `SAVESYNC-OK`. |

---

### Task 1: `SaveSyncPlan` — the decision table

**Files:**
- Create: `native/src/core/SaveSyncPlan.{h,cpp}`, `native/tools/probe_savesync.cpp`
- Modify: `native/CMakeLists.txt`, `native/tools/run-headless-probes.sh:119`, `.github/workflows/ci.yml`

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces: the whole header below. Tasks 2–5 depend on these exact names.

- [ ] **Step 1: Write the header**

```cpp
// Decides what should happen to each save file when this device and the cloud disagree. PURE — no network,
// no filesystem, no QSettings — so probe_savesync can drive every row of the table as data.
//
// Saves used to ride inside the whole-app sync zip, applied wholesale with an "always take the cloud" rule at
// startup: two devices playing the same game silently lost one side's saves, and any single save write
// re-uploaded the entire bundle. This file is the replacement rule. Two of its properties are load-bearing
// and are stated as rules rather than behaviours, because their failure modes are unrecoverable:
//   * firstRun NEVER deletes, in either direction.
//   * a conflict NEVER destroys the losing copy.
#pragma once
#include <QHash>
#include <QSet>
#include <QString>
#include <QVector>

namespace SaveSyncPlan
{
    // Ranges within which two clocks are treated as agreeing. Beyond this, newest genuinely wins.
    inline constexpr qint64 kSkewWindowMs = 5000;

    // One file's state on one side. An absent file is represented by an empty `sha`, NOT by omission —
    // callers may pass it either way and plan() treats both identically.
    struct Entry
    {
        QString name;        // "Zelda.srm" / "Zelda.state1" / "Zelda.state1.png"
        QString sha;         // content hash; empty = not present on this side
        qint64  mtimeMs = 0;
        qint64  size = 0;
        QString deviceId;    // who last wrote it (remote side only); used only to break exact ties

        bool present() const { return !sha.isEmpty(); }
    };

    enum class Act
    {
        None,           // already in step
        Upload,         // local is the newer or only copy
        Download,       // remote is the newer or only copy
        Conflict,       // both changed since the baseline and differ
        DeleteRemote,   // deleted locally, with a tombstone to prove it
        DeleteLocal     // deleted remotely, with a tombstone to prove it
    };

    struct Decision
    {
        QString name;
        Act     act = Act::None;
        // Conflict only: true when the LOCAL copy keeps the real name. This is what tells the transport
        // whether it must fetch the losing remote copy before overwriting it — see the preservation rule.
        bool    localWins = false;
        QString reason;   // one line for the log; an action is never taken silently
    };

    // The whole rule. `firstRun` (no baseline exists yet) is a HARD no-delete mode.
    QVector<Decision> plan(const QHash<QString, Entry>& local,
                           const QHash<QString, Entry>& remote,
                           const QHash<QString, Entry>& baseline,
                           const QSet<QString>&         tombstones,
                           bool                         firstRun);

    // "Zelda.state1" -> "Zelda.conflict-<deviceId>-20260727-141530.state1"
    // The suffix goes before the extension so the file keeps its type, and carries BOTH the device and the
    // timestamp so two conflicts from two devices cannot collide.
    QString conflictName(const QString& name, const QString& deviceId, qint64 mtimeMs);

    // A .conflict-* artifact is local recovery only. Syncing it would multiply one conflict across every
    // device, so it is excluded from the synced set entirely.
    bool isConflictArtifact(const QString& name);
}
```

- [ ] **Step 2: Write the failing probe**

Create `native/tools/probe_savesync.cpp`:

```cpp
// Headless coverage for the save-sync decision table. Pure — no network, no Drive, no files on disk.
// Prints SAVESYNC-OK on success; any failure prints SAVESYNC-FAIL <what> and exits non-zero.
#include "SaveSyncPlan.h"

#include <QCoreApplication>
#include <cstdio>

static int failures = 0;
#define CHECK(cond, what)                                                        \
    do { if (!(cond)) { std::fprintf(stderr, "SAVESYNC-FAIL %s\n", (what)); ++failures; } } while (0)

using namespace SaveSyncPlan;

static Entry e(const QString& name, const QString& sha, qint64 mtimeMs = 1000,
               const QString& dev = QStringLiteral("devA"))
{
    Entry x; x.name = name; x.sha = sha; x.mtimeMs = mtimeMs; x.size = sha.size(); x.deviceId = dev;
    return x;
}
static QHash<QString, Entry> one(const Entry& x) { QHash<QString, Entry> h; h.insert(x.name, x); return h; }

static Act actFor(const QVector<Decision>& ds, const QString& name)
{
    for (const Decision& d : ds) if (d.name == name) return d.act;
    return Act::None;
}
static const Decision* decFor(const QVector<Decision>& ds, const QString& name)
{
    for (const Decision& d : ds) if (d.name == name) return &d;
    return nullptr;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const QString N = QStringLiteral("Zelda.srm");

    // ------------------------------------------------- 1. the core table
    {
        // unchanged locally, changed remotely -> Download
        CHECK(actFor(plan(one(e(N, "a")), one(e(N, "b", 2000)), one(e(N, "a")), {}, false), N) == Act::Download,
              "remote-only change downloads");
        // changed locally, unchanged remotely -> Upload
        CHECK(actFor(plan(one(e(N, "b", 2000)), one(e(N, "a")), one(e(N, "a")), {}, false), N) == Act::Upload,
              "local-only change uploads");
        // neither changed -> None
        CHECK(actFor(plan(one(e(N, "a")), one(e(N, "a")), one(e(N, "a")), {}, false), N) == Act::None,
              "no change does nothing");
        // both changed, DIFFERENT content -> Conflict
        CHECK(actFor(plan(one(e(N, "b", 3000)), one(e(N, "c", 2000)), one(e(N, "a")), {}, false), N) == Act::Conflict,
              "both changed and differ is a conflict");
        // both changed to the SAME content -> None (they independently reached the same bytes)
        CHECK(actFor(plan(one(e(N, "z", 3000)), one(e(N, "z", 2000)), one(e(N, "a")), {}, false), N) == Act::None,
              "both changed to identical bytes is not a conflict");
    }

    // ------------------------------------------------- 2. absence is a restore unless a tombstone says otherwise
    {
        const QHash<QString, Entry> gone;   // local missing entirely
        CHECK(actFor(plan(gone, one(e(N, "a")), one(e(N, "a")), {}, false), N) == Act::Download,
              "a locally-missing file with NO tombstone is a restore, never a delete");
        CHECK(actFor(plan(gone, one(e(N, "a")), one(e(N, "a")), { N }, false), N) == Act::DeleteRemote,
              "…with a tombstone it is a real delete");
        CHECK(actFor(plan(one(e(N, "a")), gone, one(e(N, "a")), {}, false), N) == Act::Upload,
              "a remotely-missing file with NO tombstone is re-uploaded, never deleted locally");
        CHECK(actFor(plan(one(e(N, "a")), gone, one(e(N, "a")), { N }, false), N) == Act::DeleteLocal,
              "…with a tombstone it is a real delete");
    }

    // ------------------------------------------------- 3. THE SAFETY RULE: firstRun never deletes
    {
        const QHash<QString, Entry> gone;
        const QHash<QString, Entry> noBaseline;
        // Every shape that would otherwise delete, with firstRun set:
        for (const auto& tomb : { QSet<QString>{}, QSet<QString>{ N } })
        {
            const QVector<Decision> a = plan(gone, one(e(N, "a")), noBaseline, tomb, true);
            const QVector<Decision> b = plan(one(e(N, "a")), gone, noBaseline, tomb, true);
            CHECK(actFor(a, N) != Act::DeleteRemote && actFor(a, N) != Act::DeleteLocal,
                  "firstRun never deletes (cloud-only file)");
            CHECK(actFor(b, N) != Act::DeleteRemote && actFor(b, N) != Act::DeleteLocal,
                  "firstRun never deletes (local-only file)");
        }
        // …and it still moves data in both directions.
        CHECK(actFor(plan(gone, one(e(N, "a")), noBaseline, {}, true), N) == Act::Download,
              "firstRun downloads a cloud-only save");
        CHECK(actFor(plan(one(e(N, "a")), gone, noBaseline, {}, true), N) == Act::Upload,
              "firstRun uploads a local-only save");
    }

    // ------------------------------------------------- 4. conflict winner + localWins
    {
        // Local is newer by well over the skew window.
        const Decision* d = decFor(plan(one(e(N, "b", 90000)), one(e(N, "c", 10000)), one(e(N, "a")), {}, false), N);
        CHECK(d && d->act == Act::Conflict && d->localWins, "the newer LOCAL copy wins the real name");
        // Remote is newer. localWins must be FALSE — this is the case where the transport has to fetch the
        // losing remote copy BEFORE overwriting it, so getting this flag wrong destroys a save.
        const Decision* r = decFor(plan(one(e(N, "b", 10000)), one(e(N, "c", 90000)), one(e(N, "a")), {}, false), N);
        CHECK(r && r->act == Act::Conflict && !r->localWins, "the newer REMOTE copy wins the real name");
    }

    // ------------------------------------------------- 5. clock skew: both devices must agree
    {
        // Inside the skew window the mtimes are a tie, broken deterministically on sha then deviceId.
        Entry L = e(N, "aaa", 10000, QStringLiteral("devA"));
        Entry R = e(N, "bbb", 10000 + kSkewWindowMs - 1, QStringLiteral("devB"));
        const Decision* d1 = decFor(plan(one(L), one(R), one(e(N, "base")), {}, false), N);
        // Now compute the SAME conflict from the other device's point of view: its local is what was remote.
        const Decision* d2 = decFor(plan(one(R), one(L), one(e(N, "base")), {}, false), N);
        CHECK(d1 && d2 && d1->act == Act::Conflict && d2->act == Act::Conflict, "a near-tie is still a conflict");
        // If A says "local wins" then B, looking at the same pair, must say "local LOSES" — i.e. the two
        // devices must pick the SAME physical copy. Disagreement here means both keep their own and diverge
        // forever, which is worse than having no rule at all.
        CHECK(d1->localWins != d2->localWins, "both devices independently choose the same winner");

        // Outside the window, time decides and the tie-break must not interfere.
        Entry Old = e(N, "zzz", 10000, QStringLiteral("devZ"));           // lexically greatest sha
        Entry New = e(N, "aaa", 10000 + kSkewWindowMs + 1, QStringLiteral("devA"));
        const Decision* t = decFor(plan(one(New), one(Old), one(e(N, "base")), {}, false), N);
        CHECK(t && t->localWins, "outside the skew window the newer file wins regardless of sha");
    }

    // ------------------------------------------------- 6. in the baseline, gone from both sides
    {
        const QHash<QString, Entry> gone;
        const QVector<Decision> ds = plan(gone, gone, one(e(N, "a")), {}, false);
        CHECK(decFor(ds, N) == nullptr || actFor(ds, N) == Act::None,
              "a file gone from BOTH sides yields no action — it is not a deletion");
    }

    // ------------------------------------------------- 7. names
    {
        const QString c = conflictName(QStringLiteral("Zelda.state1"), QStringLiteral("devA"), 0);
        CHECK(c.endsWith(QStringLiteral(".state1")), "the conflict copy keeps its extension");
        CHECK(c.contains(QStringLiteral("devA")), "…and names the device that lost");
        CHECK(c != conflictName(QStringLiteral("Zelda.state1"), QStringLiteral("devB"), 0),
              "two devices' conflict copies cannot collide");
        CHECK(isConflictArtifact(c), "a conflict copy is recognised as an artifact");
        CHECK(!isConflictArtifact(QStringLiteral("Zelda.state1")), "an ordinary save is not");
        // The artifact must never be planned for sync — it is local recovery only.
        const QVector<Decision> ds = plan(one(e(c, "x")), {}, {}, {}, false);
        CHECK(decFor(ds, c) == nullptr || actFor(ds, c) == Act::None, "a conflict artifact is never synced");
    }

    if (failures) { std::fprintf(stderr, "SAVESYNC-FAIL %d check(s) failed\n", failures); return 1; }
    std::printf("SAVESYNC-OK\n");
    return 0;
}
```

- [ ] **Step 3: Create the sources and wire the build IN THE SAME STEP**

Create `native/src/core/SaveSyncPlan.cpp` containing only `#include "SaveSyncPlan.h"` for now (the stub keeps the reconfigure valid — see Global Constraints), then edit `native/CMakeLists.txt`.

Add to `qt_add_executable(everythingbox …)`:

```cmake
        src/core/SaveSyncPlan.cpp src/core/SaveSyncPlan.h
        src/core/SaveSync.cpp     src/core/SaveSync.h
        src/core/SaveMeta.cpp     src/core/SaveMeta.h
```

> `SaveSync` and `SaveMeta` are Tasks 2 and 4. Create all three `.cpp`/`.h` pairs now, the latter two as include-only stubs, so the app target keeps linking between tasks.

Add the probe beside `probe_stremio`:

```cmake
    # Headless test for the save-sync decision table. Pure — no Drive, no filesystem.
    add_executable(probe_savesync tools/probe_savesync.cpp
        src/core/SaveSyncPlan.cpp src/core/SaveSyncPlan.h)
    target_include_directories(probe_savesync PRIVATE src src/core)
    target_link_libraries(probe_savesync PRIVATE Qt6::Core)
```

Append `"probe_savesync SAVESYNC-OK"` to the runner list at `native/tools/run-headless-probes.sh:119`, and `probe_savesync` to the `--target` list in `.github/workflows/ci.yml`.

- [ ] **Step 4: Reconfigure, build, and watch it FAIL**

```bash
export PATH="/c/Qt/6.8.3/msvc2022_64/bin:/c/mpv-dev:$PATH" && cmake -S native -B build -DEVERYTHINGBOX_BUILD_APP=ON && cmake --build build --config Release --target probe_savesync
```
Expected: **link errors** — `plan`, `conflictName` and `isConflictArtifact` are declared but not defined. (The probe itself must compile clean; if it doesn't, the header is wrong, not the implementation.)

- [ ] **Step 5: Implement `SaveSyncPlan.cpp`**

```cpp
#include "SaveSyncPlan.h"

#include <QDateTime>
#include <QFileInfo>

namespace {

using SaveSyncPlan::Entry;

// Two writes within kSkewWindowMs are treated as simultaneous: wall clocks on two machines are not
// trustworthy to finer than that, and pretending otherwise makes the winner depend on clock drift.
bool nearlySimultaneous(qint64 a, qint64 b)
{
    return qAbs(a - b) < SaveSyncPlan::kSkewWindowMs;
}

// TRUE when the local copy should keep the real name. Both devices run this over the SAME pair of entries
// (with local/remote swapped), so it must be antisymmetric — if it ever returned true for both views the two
// devices would each keep their own copy and diverge permanently, which is worse than having no rule.
bool localWinsOver(const Entry& local, const Entry& remote)
{
    if (!nearlySimultaneous(local.mtimeMs, remote.mtimeMs)) return local.mtimeMs > remote.mtimeMs;
    if (local.sha != remote.sha)           return local.sha > remote.sha;
    return local.deviceId > remote.deviceId;
}

} // namespace

QVector<SaveSyncPlan::Decision> SaveSyncPlan::plan(const QHash<QString, Entry>& local,
                                                   const QHash<QString, Entry>& remote,
                                                   const QHash<QString, Entry>& baseline,
                                                   const QSet<QString>&         tombstones,
                                                   bool                         firstRun)
{
    QVector<Decision> out;

    QSet<QString> names;
    for (auto it = local.constBegin();    it != local.constEnd();    ++it) names.insert(it.key());
    for (auto it = remote.constBegin();   it != remote.constEnd();   ++it) names.insert(it.key());
    for (auto it = baseline.constBegin(); it != baseline.constEnd(); ++it) names.insert(it.key());

    QStringList ordered(names.constBegin(), names.constEnd());
    ordered.sort();   // deterministic output; the caller logs these and probes compare them

    for (const QString& name : ordered)
    {
        if (isConflictArtifact(name)) continue;   // local recovery only — never synced

        const Entry L = local.value(name);
        const Entry R = remote.value(name);
        const Entry B = baseline.value(name);

        // Gone from both sides: nothing to move and nothing to delete. Treating a stale baseline entry as a
        // deletion would let it delete a file that no longer exists anywhere.
        if (!L.present() && !R.present()) continue;

        const bool localChanged  = L.sha != B.sha;
        const bool remoteChanged = R.sha != B.sha;

        Decision d;
        d.name = name;

        if (!L.present())
        {
            // Absent locally. A tombstone is the ONLY evidence that this was a deliberate delete; without
            // one, a missing file is a restore. Getting this backwards deletes saves the user still wants.
            if (!firstRun && tombstones.contains(name))
            { d.act = Act::DeleteRemote; d.reason = QStringLiteral("deleted locally (tombstoned)"); }
            else
            { d.act = Act::Download; d.reason = firstRun ? QStringLiteral("first run: cloud-only save")
                                                         : QStringLiteral("missing locally, no tombstone"); }
            out.push_back(d);
            continue;
        }
        if (!R.present())
        {
            if (!firstRun && tombstones.contains(name))
            { d.act = Act::DeleteLocal; d.reason = QStringLiteral("deleted remotely (tombstoned)"); }
            else
            { d.act = Act::Upload; d.reason = firstRun ? QStringLiteral("first run: local-only save")
                                                       : QStringLiteral("missing remotely, no tombstone"); }
            out.push_back(d);
            continue;
        }

        if (L.sha == R.sha) { continue; }                       // already identical, whatever the baseline said

        if (localChanged && remoteChanged)
        {
            d.act = Act::Conflict;
            d.localWins = localWinsOver(L, R);
            d.reason = QStringLiteral("both changed since last sync; %1 copy is newer")
                           .arg(d.localWins ? QStringLiteral("local") : QStringLiteral("cloud"));
        }
        else if (localChanged)  { d.act = Act::Upload;   d.reason = QStringLiteral("changed on this device"); }
        else if (remoteChanged) { d.act = Act::Download; d.reason = QStringLiteral("changed on another device"); }
        else
        {
            // Neither differs from the baseline yet they differ from each other — the baseline is stale or
            // wrong. Treat it as a conflict rather than guessing: the preservation rule keeps both copies.
            d.act = Act::Conflict;
            d.localWins = localWinsOver(L, R);
            d.reason = QStringLiteral("baseline disagrees with both sides");
        }
        out.push_back(d);
    }
    return out;
}

QString SaveSyncPlan::conflictName(const QString& name, const QString& deviceId, qint64 mtimeMs)
{
    const QFileInfo fi(name);
    const QString base = fi.completeBaseName();
    const QString ext  = fi.suffix();
    const QString when = QDateTime::fromMSecsSinceEpoch(mtimeMs, Qt::UTC)
                             .toString(QStringLiteral("yyyyMMdd-HHmmss"));
    QString out = base + QStringLiteral(".conflict-") + deviceId + QLatin1Char('-') + when;
    if (!ext.isEmpty()) out += QLatin1Char('.') + ext;
    return out;
}

bool SaveSyncPlan::isConflictArtifact(const QString& name)
{
    return name.contains(QStringLiteral(".conflict-"));
}
```

- [ ] **Step 6: Build and run — expect PASS**

```bash
export PATH="/c/Qt/6.8.3/msvc2022_64/bin:/c/mpv-dev:$PATH" && cmake --build build --config Release --target probe_savesync && ./build/Release/probe_savesync.exe
```
Expected: `SAVESYNC-OK`, exit 0.

- [ ] **Step 7: Mutation-test the safety rule**

The `firstRun` no-delete rule is the one whose failure is unrecoverable, so prove the probe actually catches it. Temporarily change the two `!firstRun && tombstones.contains(name)` guards to drop the `!firstRun` term, rebuild, run.
Expected: **SAVESYNC-FAIL** on the firstRun checks. Then revert, rebuild, confirm `SAVESYNC-OK` again. Report both outputs.

- [ ] **Step 8: Confirm the app still links, then commit**

```bash
export PATH="/c/Qt/6.8.3/msvc2022_64/bin:/c/mpv-dev:$PATH" && cmake --build build --config Release --target everythingbox
git add native/src/core/SaveSyncPlan.h native/src/core/SaveSyncPlan.cpp native/src/core/SaveSync.h native/src/core/SaveSync.cpp native/src/core/SaveMeta.h native/src/core/SaveMeta.cpp native/tools/probe_savesync.cpp native/CMakeLists.txt native/tools/run-headless-probes.sh .github/workflows/ci.yml
git commit -m "feat: SaveSyncPlan — the per-file save decision table"
```

---

### Task 2: `SaveSync` — the transport

**Files:**
- Modify: `native/src/core/SaveSync.{h,cpp}` (stubs from Task 1)

**Interfaces:**
- Consumes: `SaveSyncPlan::{Entry, Act, Decision, plan, conflictName, isConflictArtifact}`; `CloudSync::{ensureFolder, findFile, uploadFile, downloadFile}` (`native/src/core/CloudSync.h:55-66`); `Tombstones::{add, all, has}` (`native/src/core/Tombstones.h`); `AppPaths::dataDir()`.
- Produces: `SaveSync` with `syncNow`, `markDirty`, `flush`, and a `conflictKept` signal. Task 5 wires these.

- [ ] **Step 1: Write the header**

```cpp
// Per-file sync of emulator saves and save states against Drive, using CloudSync's primitives. Replaces the
// old whole-zip snapshot: one Drive file per save, so an F2 press uploads ONE state instead of re-uploading
// addons, themes and settings alongside it.
//
// The rules live in SaveSyncPlan; this file is transport and ordering only. One ordering requirement here is
// load-bearing: when the REMOTE copy loses a conflict, its bytes exist only in the cloud and we are about to
// overwrite that name — so we download the loser and write it as .conflict-… BEFORE uploading the winner.
// Uploading first would make "the loser is preserved" a lie in exactly the case that matters.
//
// NOT THREAD-SAFE: hashes files and writes JSON on the calling thread. GUI-thread use only.
#pragma once
#include "SaveSyncPlan.h"   // Entry/Act/Decision are in this class's private interface

#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <functional>

class CloudSync;
class QTimer;

class SaveSync : public QObject
{
    Q_OBJECT
public:
    // `root` is the folder holding saves/ and states/ (AppPaths::dataDir()); `deviceId` is the same id the
    // progress sync uses (mdsync T1), so conflict copies name a device the user can recognise.
    SaveSync(CloudSync* cloud, QString root, QString deviceId, QObject* parent = nullptr);

    // Full reconcile: pull the manifest, plan, execute. cb reports how many files moved each way and how
    // many conflicts were preserved.
    void syncNow(std::function<void(bool ok, int uploaded, int downloaded, int conflicts)> cb);

    // A save was just written. Coalesces into one debounced upload of THAT file (kUploadDebounceMs).
    void markDirty(const QString& relPath);
    // Push anything still pending. Called at exit, inside the existing shutdown watchdog.
    void flush(std::function<void(bool ok)> cb);

    // Record that the user deliberately deleted a save, so it is not restored on the next sync.
    void recordDelete(const QString& relPath);

signals:
    // A conflict was resolved. `keptAs` is the preserved losing copy's filename; `title` is the game name
    // from SaveMeta when known, else the raw filename.
    void conflictKept(const QString& title, const QString& keptAs);
    void log(const QString& line);

private:
    QHash<QString, SaveSyncPlan::Entry> scanLocal() const;   // hashes saves/ and states/
    QHash<QString, SaveSyncPlan::Entry> readBaseline() const;
    bool writeBaseline(const QHash<QString, SaveSyncPlan::Entry>&) const;

    CloudSync* cloud_ = nullptr;
    QString    root_, deviceId_;
    QTimer*    debounce_ = nullptr;
    QSet<QString> dirty_;
};
```

- [ ] **Step 2: Implement the pieces, in this order**

1. **`scanLocal`** — walk `<root>/saves` and `<root>/states`, skipping `isConflictArtifact` names, producing `Entry{name = "saves/x.srm" | "states/x.state1", sha = SHA-256 of contents, mtimeMs, size}`. The relative form with the subdirectory prefix IS the sync name; keep it stable, it is the manifest key.
2. **`readBaseline` / `writeBaseline`** — JSON at `<root>/save-baseline.json`, same object-of-objects shape as the manifest. Use **`QSaveFile`** (this is the file that tells the next sync what already happened; a truncated one would look like `firstRun` and could re-upload everything). `firstRun` is `true` exactly when this file does not exist.
3. **The remote manifest** — `saves-index.json` in the Drive app folder via `findFile`/`downloadFile`/`uploadFile`. Same shape plus `deviceId` per entry.
4. **`syncNow`** — `ensureFolder` → fetch manifest → `scanLocal` → `readBaseline` → `Tombstones::all("saves")` → `SaveSyncPlan::plan(...)` → execute each decision → rewrite the baseline from what actually succeeded (**never from the plan** — a failed upload must not be recorded as synced).
5. **Executing a `Conflict`** — the ordering is the whole preservation promise, so it is written out here rather than described:

```cpp
void SaveSync::executeConflict(const SaveSyncPlan::Decision& d,
                               const SaveSyncPlan::Entry& localE,
                               const SaveSyncPlan::Entry& remoteE,
                               const QString& folderId,
                               std::function<void(bool ok)> done)
{
    const QString localPath = root_ + QLatin1Char('/') + d.name;

    if (!d.localWins)
    {
        // The LOCAL copy loses. Its bytes are already on this disk, so preserving it is a rename — do that
        // FIRST, then bring the winner down. If the rename fails we must NOT download, or we would overwrite
        // the copy we just failed to save.
        const QString kept = SaveSyncPlan::conflictName(d.name, deviceId_, localE.mtimeMs);
        if (!QFile::rename(localPath, root_ + QLatin1Char('/') + kept))
        { emit log(QStringLiteral("save conflict: could not preserve local copy of %1").arg(d.name)); done(false); return; }
        emit conflictKept(SaveMeta::titleFor(d.name), kept);
        downloadInto(folderId, d.name, localPath, done);
        return;
    }

    // The REMOTE copy loses — and this is the case that can destroy data. Its bytes exist ONLY in the cloud,
    // and the very next thing we do is overwrite that name with ours. So fetch the loser and write it as a
    // .conflict-… copy BEFORE uploading. Uploading first would make "the loser is preserved" a lie in exactly
    // the case that matters, and it would look correct in review.
    const QString kept = SaveSyncPlan::conflictName(d.name, remoteE.deviceId, remoteE.mtimeMs);
    downloadInto(folderId, d.name, root_ + QLatin1Char('/') + kept, [this, d, kept, localPath, folderId, done](bool ok) {
        if (!ok)
        { emit log(QStringLiteral("save conflict: could not preserve the cloud copy of %1 — NOT uploading").arg(d.name)); done(false); return; }
        emit conflictKept(SaveMeta::titleFor(d.name), kept);
        uploadFrom(folderId, d.name, localPath, done);
    });
}
```

   `downloadInto(folderId, name, destPath, cb)` and `uploadFrom(folderId, name, srcPath, cb)` are thin private helpers over `CloudSync::downloadFile`/`uploadFile` plus the manifest lookup; write them first.

   **Both failure paths abort rather than continue.** A conflict we could not preserve must leave both sides exactly as they were and retry next sync — never partially applied.
6. **Torn-write guard** — before uploading, hash the file; after reading its bytes, hash again. If they differ, abandon that upload and leave it dirty for the next pass. A half-written save state must never be published.
7. **`markDirty` / `flush`** — a single-shot `QTimer` at `kUploadDebounceMs = 10000`; on fire, upload only the dirty entries and update those baseline rows.

- [ ] **Step 3: Build and run the suite**

```bash
export PATH="/c/Qt/6.8.3/msvc2022_64/bin:/c/mpv-dev:$PATH" && cmake --build build --config Release --target everythingbox probe_savesync && BUILD_DIR=build bash native/tools/run-headless-probes.sh
```
Expected: clean build, `ALL HEADLESS PROBES PASSED`.

- [ ] **Step 4: Commit**

```bash
git add native/src/core/SaveSync.h native/src/core/SaveSync.cpp
git commit -m "feat: per-file save transport with conflict preservation ordering"
```

---

### Task 3: Take saves out of the bundle

The change that stops one save write re-uploading everything.

**Files:**
- Modify: `native/src/core/CloudSync.cpp` — `buildBundle` (`:491-492`), `applyBundle` (`:502+`), `stateHash` (`:563-565`)

**Interfaces:**
- Consumes: nothing new.
- Produces: no new API — a behaviour change Task 5 depends on.

- [ ] **Step 1: Stop writing them into the zip**

Delete the two `zipAddDir` lines for `saves` and `states` at `CloudSync.cpp:491-492`, replacing them with a comment recording why:

```cpp
    // saves/ and states/ are NOT in the bundle: they sync per-file via SaveSync. They used to be here, which
    // meant (a) two devices silently overwrote each other's saves wholesale, and (b) every save write flipped
    // the fingerprint below and re-uploaded addons, themes and settings along with it.
```

- [ ] **Step 2: Stop applying them from the zip**

In `applyBundle`, skip any entry whose path starts with `saves/` or `states/`. **This must skip, not delete** — an older device may still be uploading bundles that contain them, and extracting one would overwrite a save the per-file sync just resolved.

- [ ] **Step 3: Take them out of the fingerprint**

At `CloudSync.cpp:563-565`, remove `saves` and `states` from the iterated subdirectory list, leaving `addons` and `themes`.

- [ ] **Step 4: Build and run the suite**

```bash
export PATH="/c/Qt/6.8.3/msvc2022_64/bin:/c/mpv-dev:$PATH" && cmake --build build --config Release --target everythingbox && BUILD_DIR=build bash native/tools/run-headless-probes.sh
```
Expected: clean build, `ALL HEADLESS PROBES PASSED`.

- [ ] **Step 5: Commit**

```bash
git add native/src/core/CloudSync.cpp
git commit -m "fix: saves leave the sync bundle and its fingerprint"
```

---

### Task 4: `saveDir`, the stray sweep, and the sidecar

**Files:**
- Modify: `native/src/core/SaveMeta.{h,cpp}` (stubs from Task 1)
- Modify: `native/src/emu/RetroView.cpp` — beside the `systemDir` assignment (`:703`), and the save/state write paths (`:1373-1420`)

**Interfaces:**
- Consumes: `AppPaths::dataDir()`.
- Produces: `SaveMeta::{put, lookup, titleFor}`. Task 5 uses `titleFor` for the conflict notice.

- [ ] **Step 1: Assign `saveDir`**

At `RetroView.cpp:703`, beside `core_.systemDir`:

```cpp
    // Point the core at <data>/saves for the save files it writes ITSELF (memory cards, .brm, .smpc — the
    // ones the frontend does not manage through RETRO_MEMORY_SAVE_RAM). This was never assigned, so saveDir
    // defaulted to "." and those files landed in the working directory: the deployed install has loose
    // *.smpc files at its root, outside saves/ and therefore never backed up.
    core_.saveDir = CoreManager::savesDir().toStdString();
```

> If `CoreManager` has no `savesDir()`, add one next to `systemDir()` returning `AppPaths::dataDir() + "/saves"`, creating it if absent. Match `systemDir()`'s shape exactly.

- [ ] **Step 2: The one-time stray sweep**

Run once (guarded by a settings flag, e.g. `saves/strays.swept`), on startup before any core loads: move files from the application directory into `<data>/saves/` when the name matches the allowlist `.srm .sav .brm .smpc .mcd .mcr .eep .fla .state`.

Requirements, all load-bearing:
- **Allowlist by extension only.** Never move anything else out of the install directory.
- **Skip any file currently open** (a failed rename is a skip, not an error).
- **Never overwrite** an existing file in `saves/` — if the destination exists, leave the stray alone and log it.
- **Log every move**, so a user who wonders where a file went has a thread to pull.

- [ ] **Step 3: The sidecar**

`SaveMeta` — JSON at `<data>/saves-meta.json`, `{ "<relPath>": {title, system, romPath, updatedAt} }`, `QSaveFile` on write:

```cpp
namespace SaveMeta
{
    void    put(const QString& relPath, const QString& title, const QString& system, const QString& romPath);
    // The display title for a save file, or the bare filename when nothing is recorded. NEVER empty.
    QString titleFor(const QString& relPath);
}
```

Call `put` wherever a `.srm` or `.state<N>` is written in `RetroView.cpp` (`saveSram` `:1384`, `saveState` `:1400+`), passing the game title and system the view already holds.

**Existing files are not renamed.** A save with no sidecar entry displays its filename — the sidecar is additive. This exists because saves are keyed to the ROM's `completeBaseName`, and cached remote ROMs use 40-hex names, so most real `.srm` files are named for a ROM that may no longer exist.

- [ ] **Step 3b: System namespacing for NEW saves — with a legacy fallback that is not optional**

The spec calls for namespacing new saves by system so two systems sharing a ROM base name stop colliding. Done naively this **loses saves**: today's path is `<data>/saves/<base>.srm`, and if the code starts writing and reading `<data>/saves/<system>/<base>.srm`, every existing save becomes invisible and the game starts fresh.

So the resolution rule is, in order:

```cpp
// Where a save lives, tolerating both layouts. NEW saves are written under <saves>/<system>/ so two systems
// sharing a ROM base name stop colliding — but a save written before that change lives flat in <saves>/, and
// silently failing to find it would look to the user exactly like their save being wiped. So: prefer the
// namespaced path, fall back to the legacy flat path when it exists, and only ever CREATE namespaced.
QString RetroView::resolveSavePath(const QString& base, const QString& ext) const
{
    const QString nsPath  = savesRoot() + QLatin1Char('/') + systemId_ + QLatin1Char('/') + base + ext;
    const QString oldPath = savesRoot() + QLatin1Char('/') + base + ext;
    if (QFileInfo::exists(nsPath))  return nsPath;
    if (QFileInfo::exists(oldPath)) return oldPath;   // keep writing where the existing save already is
    return nsPath;                                     // brand new -> namespaced
}
```

**A legacy save is left where it is and keeps being used.** Migrating it would mean moving a file the user's other device may still be syncing under the old name mid-upgrade. Namespacing is for saves that do not exist yet.

`scanLocal` in `SaveSync` must therefore walk `saves/` **recursively** so namespaced files are included, and the relative path (`saves/<system>/<base>.srm`) is the sync name.

- [ ] **Step 4: Build, run the suite, commit**

```bash
export PATH="/c/Qt/6.8.3/msvc2022_64/bin:/c/mpv-dev:$PATH" && cmake --build build --config Release --target everythingbox && BUILD_DIR=build bash native/tools/run-headless-probes.sh
git add native/src/core/SaveMeta.h native/src/core/SaveMeta.cpp native/src/emu/RetroView.cpp
git commit -m "fix: assign saveDir, sweep stray core saves, and identify saves by game"
```

---

### Task 5: Wire it up

**Files:**
- Modify: `native/src/ui/MainWindow.{h,cpp}` — construction, startup, exit, the conflict notice
- Modify: `native/src/emu/RetroView.cpp` — signal a save was written

**Interfaces:**
- Consumes: `SaveSync::{syncNow, markDirty, flush, conflictKept}`, `SaveMeta::titleFor`.
- Produces: no new cross-task interface.

- [ ] **Step 1: Own a `SaveSync`**

In `MainWindow`, beside the existing `std::unique_ptr<SubtitleCache>`/`BingeStore`:

```cpp
    std::unique_ptr<SaveSync> saveSync_;   // per-file save/state sync (see SaveSyncPlan for the rules)
```
Construct it with the `CloudSync` instance, `AppPaths::dataDir()`, and the same device id the progress sync uses.

- [ ] **Step 2: Sync at the right moments**

- **Startup**, after sign-in state is known and after the bundle pull: `saveSync_->syncNow(...)`. It must run **after** the bundle apply, so a legacy bundle that still contains `saves/` cannot land on top of a resolved file — and Task 3 already makes `applyBundle` skip those entries.
- **On a save write**: `RetroView` signals it; `MainWindow` calls `markDirty(rel)`.
- **At exit**: `flush(...)` inside the existing shutdown watchdog in `closeEvent` (`MainWindow.cpp:9904-9912`), alongside the bundle push.

- [ ] **Step 3: Surface conflicts**

Connect `conflictKept` to a notice naming the **game**, not the filename:

```cpp
    connect(saveSync_.get(), &SaveSync::conflictKept, this, [this](const QString& title, const QString& keptAs) {
        // Say what happened and where the other copy went. A conflict the user is not told about is
        // indistinguishable from the data loss this whole track exists to remove.
        notifier_->notify(tr("%1 was saved on another device too — kept both. The older copy is %2.")
                              .arg(title, keptAs), 8000);
    });
```

- [ ] **Step 4: Build, run the suite, commit**

```bash
export PATH="/c/Qt/6.8.3/msvc2022_64/bin:/c/mpv-dev:$PATH" && cmake --build build --config Release --target everythingbox && BUILD_DIR=build bash native/tools/run-headless-probes.sh
git add native/src/ui/MainWindow.h native/src/ui/MainWindow.cpp native/src/emu/RetroView.cpp
git commit -m "feat: wire per-file save sync into startup, save writes, and exit"
```

---

### Task 6: Close-out — live verification and merge

- [ ] **Step 1: Live-verify against a throwaway**

**Never launch or modify the deployed app at `C:\EverythingBox-app` or its ini.** Copy to a scratch dir, strip `cloud/*`/`sync/*` from the throwaway ini **except** what is needed to sign in, and drive with `EB_UITEST=1` + a unique `EB_UITEST_PIPE`. **Never print or screenshot a credential value.** Read `verify-app-gui-capture.md` in the memory dir first.

1. **Bundle no longer carries saves** — sync, download the Drive zip, confirm no `saves/` or `states/` entries.
2. **One save = one upload** — press F2, confirm exactly one file appears in the Drive `saves/` folder and the bundle is not re-uploaded.
3. **Stray sweep** — put a `.smpc` in the app dir, start, confirm it moves into `saves/` and then syncs; confirm an unrelated file with a non-allowlisted extension is untouched.
4. **Round-trip** — save, sync, delete the local file, sync, confirm it comes back (a missing file with no tombstone is a restore).
5. **Deliberate delete** — delete a save through whatever path records a tombstone, sync, confirm it does **not** come back.
6. **Conflict without a race** — write a save, sync, then hand-edit `save-baseline.json` and the local file so both sides appear changed; sync; confirm the winner is correct **and the loser exists on disk** as `.conflict-…`, and that the notice names the game.
7. **First run never deletes** — with a populated cloud `saves/` folder, remove the local `save-baseline.json`, sync, and confirm **nothing** is deleted in either direction.

- [ ] **Step 2: Record the outcome in the spec**

Set `**Status:** Complete` in `docs/superpowers/specs/2026-07-27-save-sync-design.md` and add a section stating exactly which steps ran, which were deferred, and any defect found. **A true two-device concurrent conflict needs a second machine** — if unavailable, record it as user-gated. Do not claim a step passed that did not run.

- [ ] **Step 3: Merge**

```bash
git checkout main && git fetch origin && git merge origin/main --no-edit && git merge local/save-sync --no-edit
```
On a version-line conflict in `native/CMakeLists.txt` / `native/src/main.cpp`, take the **higher** patch number. If upstream touched a file this branch also changed, verify the merge by reading rather than trusting it.

- [ ] **Step 4: Build EVERY probe target on the merged tree**

```bash
export PATH="/c/Qt/6.8.3/msvc2022_64/bin:/c/mpv-dev:$PATH" && T=$(grep -o 'add_executable([[:space:]]*probe_[a-z0-9_]*' native/CMakeLists.txt | sed 's/.*(\s*//' | tr '\n' ' ') && cmake --build build --config Release --target $T everythingbox
```
Expected: exit 0. This catches a latent link break in a probe that compiles a source now depending on the new files — that class of break has been caught at a merge gate here more than once.

- [ ] **Step 5: Suite, push, delete the branch**

```bash
export PATH="/c/Qt/6.8.3/msvc2022_64/bin:/c/mpv-dev:$PATH" && BUILD_DIR=build bash native/tools/run-headless-probes.sh && git push origin main && git branch -d local/save-sync
```

- [ ] **Step 6: Redeploy and verify**

```bash
cp build/Release/EverythingBox.exe /c/EverythingBox-app/EverythingBox.exe && md5sum build/Release/EverythingBox.exe /c/EverythingBox-app/EverythingBox.exe
```

- [ ] **Step 7: Update the ledger**

Append to `.superpowers/sdd/progress.md`: the merge commit, what live verification covered versus deferred, and the follow-ups — the 17 standalone emulators, a save-management UI, and the netplay save/load desync guard.
