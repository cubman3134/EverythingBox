# Media-Playback Launch Supersession Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make every non-game play surface — video, audio, streams, IPTV queues, split panes, and the three readers — cancel a still-pending external emulator launch, so RPCS3 can no longer boot full-screen over a film or a book minutes later and stomp Recents with a stale PS3 entry.

**Architecture:** No new mechanism. `GameLauncher::cancelPendingEmulatorLaunch()` already exists and already does the right thing (two-phase demote/cancel, queued terminal signal, toast, no-op when idle). This change adds one named `MainWindow` helper that wraps it and twelve call sites, placed by two rules: media entry points get the call at the **top of the function** (above the external-player handoff and the split-pane early return); readers get it in `presentBook()`/`presentPdf()`/`presentComic()`, which are reached **only after** the reader has accepted the file.

**Tech Stack:** C++17, Qt 6.8.3 (Widgets + QML), CMake multi-config (Visual Studio generator), the repo's headless probe suite.

**Spec:** [`docs/superpowers/specs/2026-08-19-media-playback-supersession-design.md`](../specs/2026-08-19-media-playback-supersession-design.md)

## Global Constraints

- **No AI attribution in commits.** No `Co-Authored-By: Claude` trailer, no "Generated with Claude Code" line, no tool name in the body. Repo rule, `CLAUDE.md`.
- **Conventional commit prefixes** (`feat:`, `fix:`, `docs:`, `refactor:`) per `CONTRIBUTING.md`.
- **Never run a target-less `cmake --build build`.** `native/CMakeLists.txt` declares 52 probe harnesses; the default target builds all of them. Always name the target.
- **`--config Release`** on every build: the Windows generator is multi-config, and a Release-configured tree with a Debug-built probe reads as "not built" to the suite.
- **Qt and libmpv must be on `PATH`** before building or running the gate: `export PATH="/c/Qt/6.8.3/msvc2022_64/bin:/c/mpv-dev:$PATH"`.
- **The pre-commit hook bumps the patch version** in `native/CMakeLists.txt` and `native/src/main.cpp` and stages them. Those two files appearing in your commit is expected — do not revert or fight it.
- **The gate must end in `ALL HEADLESS PROBES PASSED`.** Anything else is a failing branch no matter how unrelated it looks.
- **`isGame` stays excluded everywhere.** An external launch must never supersede another external launch — `runEmulator`'s `emu_->busy()` refusal is deliberate, and superseding would let two RPCS3 install runs race on the same emulator directory.

## Why there is no red-green test cycle here

Say this out loud rather than fake it: this change is UI wiring inside `MainWindow`, which the repo has no headless seam for. The only pure decision in the mechanism, `LaunchCancel::decide`, was already extracted, probe-covered (`probe_launchcancel`) and mutation-tested (`native/tools/mutate-launchcancel.json`) by the parent change, and this plan does not touch it. Adding a source-grep "gate" that asserts each sink calls the helper would be exactly the kind of harness that reports green without meaning it — this repo has a documented history of six of those.

What replaces the cycle, per task:

1. a **static site check** (`grep -n`, with the exact expected occurrence count stated) confirming the diff landed at every named site and nowhere else;
2. the **compile** (`--target everythingbox`);
3. the **full probe gate**, once, at the end of the last code task — `run-headless-probes.sh` includes source-scanning probes that read `MainWindow.cpp`, so it is not a no-op for this diff;
4. an **optional live check** in Task 4, which is the only thing that exercises the actual behaviour.

## File Structure

| File | Responsibility |
|---|---|
| `native/src/ui/MainWindow.h` | Declares `supersedePendingExternalLaunch()`, beside `notePlaybackStart()` (`:963`) |
| `native/src/ui/MainWindow.cpp` | Defines the helper (after `notePlaybackStart`, `:3952`); 12 call sites; hoists `isGame` in `openLibraryItem`'s split block |

Nothing else changes. `EmulatorManager`, `LaunchCancel` and `GameLauncher` are untouched.

---

### Task 1: The helper, and the three reader surfaces

**Files:**
- Modify: `native/src/ui/MainWindow.h:963` (declaration)
- Modify: `native/src/ui/MainWindow.cpp:3947-3952` (definition, after `notePlaybackStart`)
- Modify: `native/src/ui/MainWindow.cpp:2250` (`presentBook`), `:2264` (`presentPdf`), `:2278` (`presentComic`)

**Interfaces:**
- Consumes: `GameLauncher::cancelPendingEmulatorLaunch()` — `bool cancelPendingEmulatorLaunch();`, public on `GameLauncher` (`native/src/launch/GameLauncher.h:95`). Returns true if a launch was actually cancelled; already emits its own toast and its terminal `failed()` queued. Callers ignore the return.
- Produces: `void MainWindow::supersedePendingExternalLaunch();` — private, no arguments, no return, no-op when nothing is pending. Tasks 2 and 3 call exactly this.

- [x] **Step 1: Declare the helper in the header**

In `native/src/ui/MainWindow.h`, find this line (`:963`):

```cpp
    void notePlaybackStart();                     // a play sink reached: keep the channel iff this IS its pick
```

Add immediately below it:

```cpp
    // A non-game play surface is about to own the screen -> cancel a still-pending external emulator launch,
    // so it cannot boot over the film / track / book minutes later. No-op when nothing is pending, so every
    // caller is unconditional. NOT called from notePlaybackStart() — see the definition for why.
    void supersedePendingExternalLaunch();
```

- [x] **Step 2: Define the helper**

In `native/src/ui/MainWindow.cpp`, find the end of `notePlaybackStart` (`:3947-3952`):

```cpp
void MainWindow::notePlaybackStart()
{
    resetSegmentState();   // every play sink reaches this hook
    if (channelAiring_) { channelAiring_ = false; channelSkips_ = 0; return; } // the channel's own pick — keep it
    if (channelActive()) exitChannel();                                         // a manual play supersedes the channel
}
```

Insert this function immediately after it:

```cpp
// A non-game play surface — a film, a track, an IPTV channel, a book, a comic, a photo, or any of them in a
// split pane — is about to own the screen, so an external launch still waiting on an install or a firmware
// update must not boot over it minutes from now. Left pending, its launched handler minimises the app
// mid-film and records the stale pendingEmu* entry into Recents and the play session over what was actually
// being played — and nothing re-stops the playback, because aboutToLaunch fired minutes earlier, before this
// surface even started. This is the non-game half of the supersession GameLauncher's in-app game tails do
// (finishLibretroLaunch / finishRetroParkLaunch); the primitive it calls is unchanged, including its queued
// terminal signal, so no host teardown re-enters the calling sink's stack frame.
//
// Deliberately NOT called from notePlaybackStart(), the shared "a play sink was reached" hook, for two
// reasons. openGamePath() reaches that hook BEFORE it routes, so a cancel there would let one external
// launch supersede another — refused, not superseded, is what runEmulator's busy-check deliberately keeps.
// And the hook is unreachable from half the surfaces that need this anyway: every splitTarget_ branch
// returns above it, the readers never call it, and openAudio's multi-select and the IPTV playQueue route
// drive setQueue directly and skip it.
void MainWindow::supersedePendingExternalLaunch()
{
    launcher_->cancelPendingEmulatorLaunch();
}
```

- [x] **Step 3: Call it from `presentBook`**

Find (`:2250`):

```cpp
void MainWindow::presentBook()
{
    captureReaderOrigin();
```

Replace with:

```cpp
void MainWindow::presentBook()
{
    captureReaderOrigin();
    // The reader now owns the screen, so supersede any pending external launch. Placed in the present*
    // functions rather than at the nine open sites on purpose: they are reached ONLY after book_/pdf_/comic_
    // has accepted the file, so a document the reader rejects ("Can't open book: …") never costs the user
    // their pending launch as well — the same reasoning that hoisted finishRetroParkLaunch's precondition
    // above its own supersede. presentPdf/presentComic carry the same call for the same reason.
    supersedePendingExternalLaunch();
```

- [x] **Step 4: Call it from `presentPdf`**

Find (`:2264`, line number now shifted by Step 3):

```cpp
void MainWindow::presentPdf()
{
    captureReaderOrigin();
```

Replace with:

```cpp
void MainWindow::presentPdf()
{
    captureReaderOrigin();
    supersedePendingExternalLaunch(); // reader owns the screen — post-accept placement, see presentBook
```

- [x] **Step 5: Call it from `presentComic`**

Find (`:2278`, line number now shifted):

```cpp
void MainWindow::presentComic()
{
    captureReaderOrigin();
```

Replace with:

```cpp
void MainWindow::presentComic()
{
    captureReaderOrigin();
    supersedePendingExternalLaunch(); // reader owns the screen — post-accept placement, see presentBook
```

- [x] **Step 6: Static site check**

Run:

```bash
grep -c "supersedePendingExternalLaunch" native/src/ui/MainWindow.cpp
```

Expected output: `4` (one definition + three reader calls). If it prints anything else, a call landed in the wrong place — find it with `grep -n` before continuing.

- [x] **Step 7: Build**

Run:

```bash
export PATH="/c/Qt/6.8.3/msvc2022_64/bin:/c/mpv-dev:$PATH" && cmake --build build --config Release --target everythingbox --parallel
```

Expected: ends with `everythingbox.vcxproj -> …\everythingbox.exe` and no `error C` lines. A full rebuild is ~41 s with `/MP`; an incremental one is far less.

- [x] **Step 8: Commit**

```bash
git add native/src/ui/MainWindow.h native/src/ui/MainWindow.cpp
git commit -m "fix: readers supersede a pending external emulator launch"
```

`native/CMakeLists.txt` and `native/src/main.cpp` will also appear in the commit — that is the version-bump hook, and it is expected.

---

### Task 2: The six media entry points

**Files:**
- Modify: `native/src/ui/MainWindow.cpp:1126` (the `StreamResolver::playQueue` lambda)
- Modify: `native/src/ui/MainWindow.cpp:3512` (`openVideoPath`), `:3588` (`openAudio`, multi-select branch), `:3614` (`openAudioPath`), `:4366` (`playStream`), `:4432` (`openAudioStream`)

**Interfaces:**
- Consumes: `void MainWindow::supersedePendingExternalLaunch();` from Task 1.
- Produces: nothing new.

All five function-level calls go at the **top of the function**, above everything. That position is load-bearing twice: `routePlay()` hands the file to VLC / MPC-HC / an Android intent and returns before the play sink is reached, and each function's own `if (splitTarget_)` branch returns before it too. A cancel placed lower would miss both routes. Unlike the readers there is nothing to sit behind — mpv loads asynchronously, so these sinks have no synchronous "the file was rejected" moment.

- [x] **Step 1: `openVideoPath`**

Find (`:3512`):

```cpp
void MainWindow::openVideoPath(const QString& path)
{
    PerfTrace::begin(QStringLiteral("open.video"));
    if (StreamResolver::isM3uRef(path)) { streams_->resolve(path, QFileInfo(path).completeBaseName()); return; } // playlist, not a plain file
```

Replace with:

```cpp
void MainWindow::openVideoPath(const QString& path)
{
    PerfTrace::begin(QStringLiteral("open.video"));
    // Top of the function, above the m3u / split-pane / external-player branches, because each of them
    // RETURNS before the play sink below: this file is about to be watched somewhere — this window, a pane,
    // or VLC — and a pending external launch must not boot over any of them. (Media sinks take the call at
    // entry; the readers take theirs post-accept in present*. See supersedePendingExternalLaunch.)
    supersedePendingExternalLaunch();
    if (StreamResolver::isM3uRef(path)) { streams_->resolve(path, QFileInfo(path).completeBaseName()); return; } // playlist, not a plain file
```

- [x] **Step 2: `openAudioPath`**

Find (`:3614`, shifted by Step 1):

```cpp
void MainWindow::openAudioPath(const QString& path)
{
    PerfTrace::begin(QStringLiteral("open.audio"));
    notePlaybackStart();               // channel guard: keep the channel iff this is its own audio pick
```

Replace with:

```cpp
void MainWindow::openAudioPath(const QString& path)
{
    PerfTrace::begin(QStringLiteral("open.audio"));
    supersedePendingExternalLaunch();  // this track is about to own the screen — see openVideoPath
    notePlaybackStart();               // channel guard: keep the channel iff this is its own audio pick
```

- [x] **Step 3: `openAudio`'s multi-select branch**

The single-select case delegates to `openAudioPath` (covered by Step 2); only the multi-select branch needs its own call, because it drives `setQueue` directly and reaches neither `openAudioPath` nor `notePlaybackStart`.

Find (`:3588`, shifted):

```cpp
    if (sel.size() == 1) { openAudioPath(sel.first()); return; } // folder queue starting at this track

    // The multi-select branch drives setQueue directly and never reaches notePlaybackStart, so clear the
    // previous file's segment state here — otherwise an episode's learned intro stays armed against track 1.
    resetSegmentState();
```

Replace with:

```cpp
    if (sel.size() == 1) { openAudioPath(sel.first()); return; } // folder queue starting at this track

    // The multi-select branch drives setQueue directly and never reaches notePlaybackStart, so clear the
    // previous file's segment state here — otherwise an episode's learned intro stays armed against track 1.
    // For the same reason it needs its own supersede: it reaches none of the sinks that carry one.
    supersedePendingExternalLaunch();
    resetSegmentState();
```

- [x] **Step 4: `playStream`**

Find (`:4366`, shifted):

```cpp
void MainWindow::playStream(const QString& url, const QString& resumeKey, const QString& title,
                            const StreamHeaders::Headers& headers)
{
    PerfTrace::begin(QStringLiteral("open.video"));
```

Replace with:

```cpp
void MainWindow::playStream(const QString& url, const QString& resumeKey, const QString& title,
                            const StreamHeaders::Headers& headers)
{
    PerfTrace::begin(QStringLiteral("open.video"));
    supersedePendingExternalLaunch(); // above the external-player handoff below — see openVideoPath
```

- [x] **Step 5: `openAudioStream`**

Find (`:4432`, shifted):

```cpp
    PerfTrace::begin(QStringLiteral("open.audio"));
    if (splitTarget_) { splitTarget_->openVideo(url, title, headers); finishSplitOpen(); return; }
    notePlaybackStart();    // channel guard: keep the channel iff this is its own audio-stream pick
```

Replace with:

```cpp
    PerfTrace::begin(QStringLiteral("open.audio"));
    // Above the split-pane branch, which returns: a pane playing this audiobook owns the screen too. This
    // one call also covers openLibraryItem's audiobook and audio leaves, which both delegate here.
    supersedePendingExternalLaunch();
    if (splitTarget_) { splitTarget_->openVideo(url, title, headers); finishSplitOpen(); return; }
    notePlaybackStart();    // channel guard: keep the channel iff this is its own audio-stream pick
```

- [x] **Step 6: The IPTV `playQueue` lambda**

Find (`:1126`) — the body starts right after the parameter list:

```cpp
        // An IPTV / media playlist: build a channel queue (the list panel + next/prev), play the first entry.
        currentNextSourceCapable_ = false;
```

Replace with:

```cpp
        // An IPTV / media playlist: build a channel queue (the list panel + next/prev), play the first entry.
        // Like openAudio's multi-select branch, this route drives setQueue directly and reaches no other
        // sink, so it carries its own supersede: a channel about to air owns the screen like any other play.
        supersedePendingExternalLaunch();
        currentNextSourceCapable_ = false;
```

- [x] **Step 7: Static site check**

Run:

```bash
grep -n "supersedePendingExternalLaunch" native/src/ui/MainWindow.cpp
```

Expected: **11** lines — 10 occurrences (the definition, three inside `presentBook`/`presentPdf`/`presentComic`, and the six added here (one in the `playQueue` lambda near the top of the file, then `openVideoPath`, `openAudio`, `openAudioPath`, `playStream`, `openAudioStream` in file order), plus one line that only mentions the name inside `openVideoPath`'s comment. Confirm each of the six sits at the top of its function/branch, above any `return`.

- [x] **Step 8: Build**

```bash
export PATH="/c/Qt/6.8.3/msvc2022_64/bin:/c/mpv-dev:$PATH" && cmake --build build --config Release --target everythingbox --parallel
```

Expected: no `error C` lines.

- [x] **Step 9: Commit**

```bash
git add native/src/ui/MainWindow.cpp
git commit -m "fix: media playback supersedes a pending external emulator launch"
```

---

### Task 3: Catalog leaves, split panes, and the gate

**Files:**
- Modify: `native/src/ui/MainWindow.cpp:4477` (`openDocumentPath`'s split branch)
- Modify: `native/src/ui/MainWindow.cpp:11064-11079` (`openLibraryItem`'s split block — hoist `isGame`, add the guarded call)
- Modify: `native/src/ui/MainWindow.cpp:11154` (`openLibraryItem`'s full-screen video leaf)

**Interfaces:**
- Consumes: `void MainWindow::supersedePendingExternalLaunch();` from Task 1.
- Produces: nothing new.

- [x] **Step 1: `openDocumentPath`'s split branch**

A pane takes the screen whether or not the file parses (the pane surfaces its own errors and `finishSplitOpen()` switches to the split view unconditionally), so the post-accept rule that governs the full-screen readers does not apply here.

Find (`:4477`, shifted by Tasks 1-2):

```cpp
    if (splitTarget_)
    {
        if (ext == QStringLiteral("pdf")) splitTarget_->openPdf(f);
```

Replace with:

```cpp
    if (splitTarget_)
    {
        // A pane takes the screen whether or not the file parses — it surfaces its own errors and
        // finishSplitOpen() switches to the split view regardless — so this branch supersedes up front
        // rather than post-accept the way the full-screen readers do in present*.
        supersedePendingExternalLaunch();
        if (ext == QStringLiteral("pdf")) splitTarget_->openPdf(f);
```

- [x] **Step 2: Hoist `isGame` in `openLibraryItem`'s split block and add the guarded call**

Find (`:11064-11079`, shifted):

```cpp
    if (splitTarget_)
    {
        if (type == QStringLiteral("ebook") || lower.endsWith(QStringLiteral(".epub")))
        { splitTarget_->openBook(url); finishSplitOpen(); return; }
        if (type == QStringLiteral("pdf") || lower.endsWith(QStringLiteral(".pdf")))
        { splitTarget_->openPdf(url); finishSplitOpen(); return; }
        if (lower.endsWith(QStringLiteral(".cbz")) || lower.endsWith(QStringLiteral(".cb7"))
            || lower.endsWith(QStringLiteral(".cbt")))
        { splitTarget_->openComic(url); finishSplitOpen(); return; }
        if (PhotoLibrary::isPhotoFile(url)) // #102: a local image opens in the pane's photo viewer
        { splitTarget_->openPhoto(url); finishSplitOpen(); return; }
        const bool isGame = (type == QStringLiteral("game")
                             || SystemCatalog::forExtension(QFileInfo(lower).suffix()) != nullptr);
        if (!isGame) // video / audio / audiobook all play through the pane's own libmpv
        { splitTarget_->openVideo(url, item.title, item.requestHeaders); finishSplitOpen(); return; }
    }
```

Replace with:

```cpp
    if (splitTarget_)
    {
        // Hoisted from the video branch below so it can gate the supersede as well: every non-game leaf in
        // this block hands the item to the pane, which then owns the screen, so all of them must cancel a
        // pending external launch. A GAME must not — it falls through to openGamePath, whose external route
        // goes back through open() -> runEmulator, where the busy-refusal is the behaviour we deliberately
        // keep (superseding would let two emulator install runs race on the same directory).
        const bool isGame = (type == QStringLiteral("game")
                             || SystemCatalog::forExtension(QFileInfo(lower).suffix()) != nullptr);
        if (!isGame) supersedePendingExternalLaunch();
        if (type == QStringLiteral("ebook") || lower.endsWith(QStringLiteral(".epub")))
        { splitTarget_->openBook(url); finishSplitOpen(); return; }
        if (type == QStringLiteral("pdf") || lower.endsWith(QStringLiteral(".pdf")))
        { splitTarget_->openPdf(url); finishSplitOpen(); return; }
        if (lower.endsWith(QStringLiteral(".cbz")) || lower.endsWith(QStringLiteral(".cb7"))
            || lower.endsWith(QStringLiteral(".cbt")))
        { splitTarget_->openComic(url); finishSplitOpen(); return; }
        if (PhotoLibrary::isPhotoFile(url)) // #102: a local image opens in the pane's photo viewer
        { splitTarget_->openPhoto(url); finishSplitOpen(); return; }
        if (!isGame) // video / audio / audiobook all play through the pane's own libmpv
        { splitTarget_->openVideo(url, item.title, item.requestHeaders); finishSplitOpen(); return; }
    }
```

Note: `isGame`'s initialiser is moved verbatim, and its single existing use (`if (!isGame)` on the video branch) is unchanged. Nothing between the old and new positions reads or writes `type`, `lower` or the catalog, so the hoist is behaviour-preserving.

- [x] **Step 3: `openLibraryItem`'s full-screen video leaf**

This leaf has its own `routePlay()` external-player handoff that returns above the play sink, so the call goes at the **top of the leaf**, not beside the teardown ritual lower down. Its audio and audiobook siblings need nothing — both delegate to `openAudioStream`, which Task 2 covered.

Find (`:11154`, shifted):

```cpp
    else // "video", "link", or anything else playable -> libmpv (handles files and http/streams)
    {
        // Resume + Recent are keyed by the item's stable id when it has one (a debrid/stream URL changes every
        // time it's resolved, so keying on the URL would lose your place and duplicate the Recent entry).
        const QString rkey = item.id.isEmpty() ? url : item.id;
```

Replace with:

```cpp
    else // "video", "link", or anything else playable -> libmpv (handles files and http/streams)
    {
        // Top of the leaf, above the routePlay handoff below, which RETURNS when an external player takes
        // the item: this is about to be watched here or in VLC either way, so a pending external launch is
        // superseded on both routes. (The audio/audiobook leaves above need no call — they delegate to
        // openAudioStream, which carries its own.)
        supersedePendingExternalLaunch();
        // Resume + Recent are keyed by the item's stable id when it has one (a debrid/stream URL changes every
        // time it's resolved, so keying on the URL would lose your place and duplicate the Recent entry).
        const QString rkey = item.id.isEmpty() ? url : item.id;
```

- [x] **Step 4: Static site check — all 12 sites**

Run:

```bash
grep -n "supersedePendingExternalLaunch" native/src/ui/MainWindow.h native/src/ui/MainWindow.cpp
```

Expected: **1** line in `MainWindow.h` (the declaration) and **14** in `MainWindow.cpp` — one definition, these twelve, and one comment mention inside `openVideoPath` — in file order: the `playQueue` lambda, `presentBook`, `presentPdf`, `presentComic`, `openVideoPath`, `openAudio`, `openAudioPath`, `playStream`, `openAudioStream`, `openDocumentPath`'s split branch, `openLibraryItem`'s split block, `openLibraryItem`'s video leaf.

Then confirm the one exclusion that matters:

```bash
grep -n "notePlaybackStart\(\);" native/src/ui/MainWindow.cpp
```

Expected: the existing call sites only, and **no** `supersedePendingExternalLaunch()` inside `notePlaybackStart`'s body (`grep -A4 "void MainWindow::notePlaybackStart" native/src/ui/MainWindow.cpp` must not show it). A cancel there would let an external launch supersede its predecessor.

- [x] **Step 5: Build**

```bash
export PATH="/c/Qt/6.8.3/msvc2022_64/bin:/c/mpv-dev:$PATH" && cmake --build build --config Release --target everythingbox --parallel
```

Expected: no `error C` lines.

- [x] **Step 6: Run the full probe gate**

```bash
export PATH="/c/Qt/6.8.3/msvc2022_64/bin:/c/mpv-dev:$PATH" && BUILD_DIR=build bash native/tools/run-headless-probes.sh
```

Expected: the run ends with

```
ALL HEADLESS PROBES PASSED
```

This is not ceremonial for this diff: the suite includes source-scanning probes that read `MainWindow.cpp`. If a probe reports "not built", build it by name per `CONTRIBUTING.md` (`cmake --build build --config Release --target probe_<name>`) and re-run — do not skip it.

- [x] **Step 7: Commit**

```bash
git add native/src/ui/MainWindow.cpp
git commit -m "fix: catalog and split-pane opens supersede a pending external launch"
```

---

### Task 4: Verify against the spec, and close out

**Files:** none modified unless a check fails.

- [x] **Step 1: Re-read the spec's choke-point table against the diff**

```bash
git diff main --stat && git diff main -- native/src/ui/MainWindow.h native/src/ui/MainWindow.cpp
```

Walk the spec's three groups and tick each of the twelve sites off in the diff. Confirm no site landed in `notePlaybackStart`, `finishSplitOpen`, or `openGamePath`.

- [x] **Step 2: Confirm nothing outside `MainWindow` changed**

```bash
git diff main --name-only
```

Expected: `native/src/ui/MainWindow.h`, `native/src/ui/MainWindow.cpp`, the spec and plan docs, and the version-bump pair (`native/CMakeLists.txt`, `native/src/main.cpp`). Any change to `EmulatorManager`, `LaunchCancel` or `GameLauncher` means the plan was exceeded — the primitive is unchanged by design.

- [ ] **Step 3 (optional, manual): live check** — NOT RUN, see the report

This is the only step that exercises the behaviour. Deploy the Release build to `C:\EverythingBox-app` and:

1. launch a PS3 game so `runPs3UpdateThenLaunch`'s worker starts (the wait page appears);
2. press F8 to enter split screen, F8 again to leave it — you land on Home with the launch still pending;
3. play any video.

Expected: the "Cancelled the pending launch of “<title>”." toast appears **immediately**, and RPCS3 never opens. If the launch was still in its download/install phase the toast also carries "The emulator download it started will finish in the background." Repeat with a book to check the reader path.

- [x] **Step 4: Report**

Report the gate output verbatim, the twelve confirmed sites, and — explicitly — whether Step 3 was run or skipped. Do not describe the behaviour as verified if only the build and the gate ran.

## Self-Review

**Spec coverage.** Readers → Task 1 (3 sites). Media entry points → Task 2 (6 sites). Catalog/split panes → Task 3 (3 sites, plus the `isGame` hoist). The helper → Task 1. "Why not `notePlaybackStart`" → encoded as the helper's comment (Task 1 Step 2) and as an explicit negative check (Task 3 Step 4). Verification section → Tasks 3 and 4. Out-of-scope items are carried by the spec and touched by no task, which is correct.

**Placeholders.** None: every code step shows the exact before and after text, every command is runnable as written, and every expected output is stated as a concrete string or count.

**Type consistency.** One new symbol, `MainWindow::supersedePendingExternalLaunch()`, spelled identically in the header declaration, the definition, all twelve call sites and all four grep checks. It consumes `GameLauncher::cancelPendingEmulatorLaunch()`, whose existing signature (`bool`, no arguments) is quoted from `GameLauncher.h:95`; the return is deliberately ignored.
