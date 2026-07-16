# Foundation Refactor — Plan 1 (MainWindow seams + synthetic browse catalogs)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Extract MainWindow's four seams (Notifier, StreamResolver, PlaybackSession, GameLauncher) and HomeView's synthetic browse-catalog builders into focused, probe-tested modules, with the app fully working after every commit.

**Architecture:** Behavior-preserving extraction — code MOVES out of `native/src/ui/MainWindow.cpp` (5,944 lines) and `native/src/ui/HomeView.cpp` (3,997 lines) into new classes; MainWindow keeps window/stack orchestration and connects to signals. Spec: `docs/superpowers/specs/2026-07-16-foundation-refactor-design.md`.

**Tech Stack:** Qt 6 Widgets C++ (existing style), headless `probe_*` executables (offscreen QPA, sentinel-on-success — the project's test pattern, see `native/tools/probe_nav.cpp`), `MMV_UITEST=1` + `native/tools/uitest.py` for live verification.

**Scope note:** This is plan 1 of the phase-1 spec. It covers spec steps 1–4 plus the pure part of step 5 (recents/downloads/favorites catalog builders). A follow-up plan covers the async-coupled providers (playlists, Steam, search, addon catalogs) and the step-6 sweep — those touch HomeView's request/response machinery and deserve their own plan.

## Global Constraints

- Qt 6, C++17, match the existing comment density and naming style (trailing `_` members, explanatory block comments).
- NEVER use QDialog/QMessageBox/QInputDialog/top-level windows for new UI — all feedback goes through the nav kit / Notifier (project rule, enforced by probe_nav conventions).
- One seam per commit. Move code, don't rewrite it — bodies transfer verbatim except for renamed member access.
- Build: `cmake --build build --config Release` from the repo root (`C:\Users\cubma\Project Goliath`). App target: `mymediavault`. Probe exes land in `build/Release/`.
- Live verification: deploy the Release exe over the copy in `C:\MyMediaVault-app` (check the existing exe name with `ls C:/MyMediaVault-app/*.exe` first and keep it), launch with `MMV_UITEST=1`, drive via `python native/tools/uitest.py`. Never use SendKeys/foreground automation.
- New source files are added to the `qt_add_executable(mymediavault ...)` source list in `native/CMakeLists.txt` (MainWindow's entry is at line ~139: `src/ui/MainWindow.cpp src/ui/MainWindow.h`).
- All log lines in extracted code keep using the `<app>/stream_debug.log` one-liner pattern; each new .cpp gets its own local `static void xxLog(const QString&)` copy of `mwLog` (MainWindow.cpp:142-147) — it's 4 lines, duplication is cheaper than a shared header for a logger.

---

### Task 1: Notifier — the one user-feedback channel

**Files:**
- Create: `native/src/ui/Notifier.h`, `native/src/ui/Notifier.cpp`
- Create: `native/tools/probe_notifier.cpp`
- Modify: `native/src/ui/MainWindow.h` (drop notice members, add `Notifier* notifier_`)
- Modify: `native/src/ui/MainWindow.cpp` (move `notify`/`hideNotice`/`positionNotice` at lines 1402-1433, `showPlayerNotice` at 1390-1400, and the constructor's creation of `notice_`, `noticeTimer_`, `playerNotice_`, `playerNoticeTimer_` — find them with `grep -n "notice_ = new\|noticeTimer_ = new\|playerNotice_ = new\|playerNoticeTimer_ = new" native/src/ui/MainWindow.cpp`)
- Modify: `native/CMakeLists.txt` (app source list + probe target)

**Interfaces:**
- Produces (later tasks depend on these exact signatures):
  - `void Notifier::notify(const QString& text, int ms = 4500)` — window-level notice, `ms <= 0` = sticky
  - `void Notifier::hideNotice()`
  - `void Notifier::reposition()` — re-anchor both overlays (call from resize/move)
  - `void Notifier::playerNotice(const QString& msg, int ms = 6000)` — centred transient over the player
  - `void Notifier::hidePlayerNotice()`
  - `void Notifier::setPlayerHost(QWidget* player, std::function<int()> topOffsetPx)` — attaches the player overlay; `topOffsetPx` supplies the current top inset (MainWindow passes `[this]{ return 16 + videoBack_->height() + 14; }`)
  - `bool Notifier::playerNoticeVisible() const`

- [ ] **Step 1: Write the failing probe**

`native/tools/probe_notifier.cpp`:

```cpp
// Headless test for the Notifier overlay (the app's single user-feedback channel): show/hide/sticky/
// reposition invariants under the offscreen QPA. Prints NOTIFIER-OK when every assert holds.
#include <QApplication>
#include <QWidget>
#include "../src/ui/Notifier.h"

static int fails = 0;
#define CHECK(cond, name) do { if (cond) printf("PASS %s\n", name); \
    else { printf("FAIL %s\n", name); ++fails; } } while (0)

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    QWidget host; host.resize(1280, 720); host.show();
    Notifier n(&host);

    n.notify(QStringLiteral("hello"), 4500);
    QWidget* notice = host.findChild<QWidget*>(QStringLiteral("mmvNotice"));
    CHECK(notice && notice->isVisible(), "notify shows the notice");
    CHECK(notice->geometry().center().x() > 400 && notice->geometry().center().x() < 880,
          "notice is horizontally centred");
    n.hideNotice();
    CHECK(!notice->isVisible(), "hideNotice hides it");

    n.notify(QStringLiteral("sticky"), 0);       // ms <= 0 = sticky (no auto-hide timer)
    CHECK(notice->isVisible(), "sticky notice shows");

    QWidget player(&host); player.setGeometry(0, 0, 1280, 720); player.show();
    n.setPlayerHost(&player, []{ return 60; });
    n.playerNotice(QStringLiteral("up next"), 6000);
    QWidget* pn = player.findChild<QWidget*>(QStringLiteral("mmvPlayerNotice"));
    CHECK(pn && pn->isVisible(), "playerNotice shows over the player");
    CHECK(n.playerNoticeVisible(), "playerNoticeVisible reports true");
    n.hidePlayerNotice();
    CHECK(!pn->isVisible(), "hidePlayerNotice hides it");

    host.resize(900, 500);
    n.reposition();                               // must not crash with both overlays live
    CHECK(true, "reposition survives a resize");

    if (fails == 0) printf("NOTIFIER-OK\n");
    return fails == 0 ? 0 : 1;
}
```

CMake (add after the `probe_nav` block, `native/CMakeLists.txt` ~line 255):

```cmake
    # Headless test for the Notifier overlay (the single user-feedback channel).
    add_executable(probe_notifier tools/probe_notifier.cpp
        src/ui/Notifier.cpp src/ui/Notifier.h)
    target_include_directories(probe_notifier PRIVATE src src/ui)
    target_link_libraries(probe_notifier PRIVATE Qt6::Widgets)
```

- [ ] **Step 2: Run it to verify it fails**

Run: `cmake --build build --config Release --target probe_notifier`
Expected: FAIL — `Notifier.h: No such file or directory`

- [ ] **Step 3: Implement Notifier**

`native/src/ui/Notifier.h`:

```cpp
#pragma once
#include <QObject>
#include <functional>

class QLabel;
class QTimer;
class QWidget;

// The app's single user-visible feedback channel: a window-level notice (bottom-centre toast used for
// download/resolve progress + errors, over ANY view) and a transient centred message over the player.
// Every failure the user should hear about routes through here — no silent failures, no popup dialogs.
class Notifier : public QObject
{
    Q_OBJECT
public:
    explicit Notifier(QWidget* windowHost, QObject* parent = nullptr);

    void notify(const QString& text, int ms = 4500); // ms <= 0 = sticky (no auto-hide)
    void hideNotice();
    void reposition();                               // re-anchor both overlays (resize / move)

    // The centred transient message over the player surface (visible in full screen). topOffsetPx
    // supplies the y inset below the player's top-left controls, queried at show time.
    void setPlayerHost(QWidget* player, std::function<int()> topOffsetPx);
    void playerNotice(const QString& msg, int ms = 6000);
    void hidePlayerNotice();
    bool playerNoticeVisible() const;

private:
    void positionNotice();
    QWidget* host_ = nullptr;          // the window's central area the notice floats over
    QLabel* notice_ = nullptr;         // objectName "mmvNotice"
    QTimer* noticeTimer_ = nullptr;
    QWidget* player_ = nullptr;
    std::function<int()> playerTop_;
    QLabel* playerNotice_ = nullptr;   // objectName "mmvPlayerNotice"
    QTimer* playerNoticeTimer_ = nullptr;
};
```

`native/src/ui/Notifier.cpp`: the constructor takes the `notice_ = new QLabel(...)` / `noticeTimer_ = new QTimer(...)` creation-and-styling lines verbatim from the MainWindow constructor (found in Step 1's grep), parented to `windowHost`; `setPlayerHost` takes the `playerNotice_` / `playerNoticeTimer_` creation lines, parented to `player`. Set `notice_->setObjectName(QStringLiteral("mmvNotice"))` and `playerNotice_->setObjectName(QStringLiteral("mmvPlayerNotice"))` so the probe (and uitest state) can find them. The method bodies move verbatim from MainWindow.cpp:

| Notifier method | moves from MainWindow.cpp |
|---|---|
| `notify` | lines 1415-1427 (`width()` → `host_->width()`) |
| `hideNotice` | lines 1429-1433 |
| `positionNotice` | lines 1402-1413 (`this` fallback → `host_`) |
| `playerNotice` | lines 1390-1400 (`player_->width()` stays; the hardcoded `margin + videoBack_->height() + 14` y-offset becomes `playerTop_()`) |
| `reposition` | new 3-liner: `positionNotice(); if (playerNotice_ && playerNotice_->isVisible()) { /* re-run the playerNotice move line */ }` |

- [ ] **Step 4: Run the probe**

Run: `cmake --build build --config Release --target probe_notifier && ./build/Release/probe_notifier.exe`
Expected: `NOTIFIER-OK`, exit 0

- [ ] **Step 5: Rewire MainWindow**

- `MainWindow.h`: delete members `notice_`, `noticeTimer_`, `playerNotice_`, `playerNoticeTimer_` and method decls `positionNotice()`, `showPlayerNotice(...)`; add `class Notifier* notifier_ = nullptr;`. KEEP the `notify(text, ms)` and `hideNotice()` slots (HomeView's `toastRequested`/`toastHideRequested` connect to them) but their bodies become one-line delegations.
- `MainWindow.cpp`: construct `notifier_ = new Notifier(this, this);` where `notice_` was created; call `notifier_->setPlayerHost(player_, [this]{ return 16 + videoBack_->height() + 14; });` where `playerNotice_` was created. Then mechanically:

```
notify(x, y)              -> notifier_->notify(x, y)        (body of the slot; other callers unchanged)
hideNotice()              -> notifier_->hideNotice()        (body of the slot)
positionNotice()          -> notifier_->reposition()        (resizeEvent / moveEvent call sites)
showPlayerNotice(x, y)    -> notifier_->playerNotice(x, y)
playerNotice_->hide()     -> notifier_->hidePlayerNotice()  (tryPlayNextEpisode:1589, playResolvedEpisode:1596)
playerNotice_ && playerNotice_->isVisible() -> notifier_->playerNoticeVisible()  (positionMediaControls:1382)
```

Find every touch point: `grep -n "notice_\|showPlayerNotice\|positionNotice" native/src/ui/MainWindow.cpp` — the list must be empty of direct member access when done (only `notifier_->` calls remain). `positionMediaControls` (line 1382-1386) drops its inline playerNotice positioning in favour of `notifier_->reposition()`.

- [ ] **Step 6: Build the app + probe suite, verify live**

Run: `cmake --build build --config Release --target mymediavault probe_notifier probe_nav`
Expected: clean build; `./build/Release/probe_notifier.exe` → `NOTIFIER-OK`; `./build/Release/probe_nav.exe` → its PASS output.

Live: deploy the exe over `C:\MyMediaVault-app`, launch with `MMV_UITEST=1`, then:
`python native/tools/uitest.py state` (app responds), `python native/tools/uitest.py shot scratchpad/notifier-smoke.png`. Drive Home → a catalog that shows a toast (any download/resolve action) if convenient; otherwise the state+shot smoke suffices — the probe covers the widget logic.

- [ ] **Step 7: Commit**

```bash
git add -A native/src/ui/Notifier.h native/src/ui/Notifier.cpp native/tools/probe_notifier.cpp native/src/ui/MainWindow.h native/src/ui/MainWindow.cpp native/CMakeLists.txt
git commit -m "refactor: extract Notifier — the one user-feedback channel (window notice + player notice)"
```

---

### Task 2: StreamResolver — M3U/stream dispatch, finally unit-testable

**Files:**
- Create: `native/src/media/StreamResolver.h`, `native/src/media/StreamResolver.cpp`
- Create: `native/tools/probe_m3u.cpp`
- Modify: `native/src/ui/MainWindow.h` (drop `openM3u`/`handleM3u` decls, add `StreamResolver* streams_`)
- Modify: `native/src/ui/MainWindow.cpp` (move lines 170-231: `M3uEntry`, `isM3uRef`, `isHlsManifest`, `parseM3u`, `looksLikeDiscPlaylist`; move `openM3u` 2192-2220 and `handleM3u` 2222-2255; also move `logSafeUrl` 151-159 — keep a copy in MainWindow.cpp, it's used elsewhere there)
- Modify: `native/CMakeLists.txt`

**Interfaces:**
- Consumes: `Notifier` (Task 1) — no direct dependency; errors surface via the `status` signal → MainWindow routes.
- Produces:
  - `struct M3uEntry { QString title; QString url; };` (public, in StreamResolver.h)
  - statics (pure, probe-tested): `bool StreamResolver::isM3uRef(const QString&)`, `bool StreamResolver::isHlsManifest(const QString&)`, `QVector<M3uEntry> StreamResolver::parseM3u(const QString& text, const QString& src)`, `bool StreamResolver::looksLikeDiscPlaylist(const QVector<M3uEntry>&)`
  - `void StreamResolver::resolve(const QString& src, const QString& title)` — reads the local file or GETs the URL, classifies, emits exactly one of:
  - signals: `playDirect(QString url, QString title)` (HLS / unparseable / fetch-failed fallback), `playQueue(QStringList urls, QStringList titles, QString recentSrc, QString title)` (IPTV/media list), `openDisc(QString src, QString title)` (PlayStation multi-disc), `status(QString message)` (e.g. "Loading playlist…")

- [ ] **Step 1: Write the failing probe**

`native/tools/probe_m3u.cpp`:

```cpp
// Headless test for StreamResolver's playlist classification: HLS manifest vs IPTV list vs
// PlayStation disc set, plus relative-URL resolution. Prints M3U-OK when every assert holds.
#include <QCoreApplication>
#include "../src/media/StreamResolver.h"

static int fails = 0;
#define CHECK(cond, name) do { if (cond) printf("PASS %s\n", name); \
    else { printf("FAIL %s\n", name); ++fails; } } while (0)

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    CHECK(StreamResolver::isM3uRef("http://x/list.m3u8?token=1"), "isM3uRef ignores the query");
    CHECK(!StreamResolver::isM3uRef("http://x/video.mp4"), "isM3uRef rejects plain media");

    CHECK(StreamResolver::isHlsManifest("#EXTM3U\n#EXT-X-TARGETDURATION:10\nseg1.ts"),
          "HLS manifest detected by #EXT-X-");
    CHECK(!StreamResolver::isHlsManifest("#EXTM3U\n#EXTINF:-1,Ch1\nhttp://a/1"),
          "plain media list is not HLS");

    const auto iptv = StreamResolver::parseM3u(
        "#EXTM3U\n#EXTINF:-1,Channel One\nhttp://srv/one\n#EXTINF:-1,Channel Two\nrel/two.ts\n",
        "http://host/pl/list.m3u");
    CHECK(iptv.size() == 2, "parseM3u finds both entries");
    CHECK(iptv[0].title == "Channel One" && iptv[0].url == "http://srv/one", "absolute entry kept");
    CHECK(iptv[1].url == "http://host/pl/rel/two.ts", "relative entry resolved against the playlist URL");
    CHECK(!StreamResolver::looksLikeDiscPlaylist(iptv), "IPTV list is not a disc set");

    const auto discs = StreamResolver::parseM3u(
        "Game (Disc 1).chd\nGame (Disc 2).chd\n", "C:/roms/psx/Game.m3u");
    CHECK(discs.size() == 2, "disc list parses");
    CHECK(StreamResolver::looksLikeDiscPlaylist(discs), "all-disc entries detected as a disc set");

    if (fails == 0) printf("M3U-OK\n");
    return fails == 0 ? 0 : 1;
}
```

CMake:

```cmake
    # Headless test for StreamResolver's m3u classification (HLS vs IPTV vs multi-disc).
    add_executable(probe_m3u tools/probe_m3u.cpp
        src/media/StreamResolver.cpp src/media/StreamResolver.h)
    target_include_directories(probe_m3u PRIVATE src src/media)
    target_link_libraries(probe_m3u PRIVATE Qt6::Core Qt6::Network)
```

- [ ] **Step 2: Run it to verify it fails**

Run: `cmake --build build --config Release --target probe_m3u`
Expected: FAIL — `StreamResolver.h: No such file or directory`

- [ ] **Step 3: Implement StreamResolver**

`native/src/media/StreamResolver.h`:

```cpp
#pragma once
#include <QObject>
#include <QVector>

class QNetworkAccessManager;

struct M3uEntry { QString title; QString url; };

// .m3u / .m3u8 playlist + stream-link dispatch. Three flavours share the extension: an HLS manifest
// (one adaptive stream libmpv chews directly), an IPTV/media list (becomes a channel queue), and a
// PlayStation multi-disc list (the emulator swaps discs itself). resolve() reads/fetches the source,
// classifies it, and emits exactly one outcome signal; the host decides what "play" means.
class StreamResolver : public QObject
{
    Q_OBJECT
public:
    explicit StreamResolver(QObject* parent = nullptr);

    // Pure classification helpers (probe-tested; see tools/probe_m3u.cpp).
    static bool isM3uRef(const QString& urlOrPath);
    static bool isHlsManifest(const QString& text);
    static QVector<M3uEntry> parseM3u(const QString& text, const QString& src);
    static bool looksLikeDiscPlaylist(const QVector<M3uEntry>& entries);

    void resolve(const QString& src, const QString& title); // local file or http(s) URL

signals:
    void playDirect(const QString& url, const QString& title);   // HLS / unparseable / fetch failed
    void playQueue(const QStringList& urls, const QStringList& titles,
                   const QString& recentSrc, const QString& title); // IPTV/media list
    void openDisc(const QString& src, const QString& title);     // PlayStation multi-disc set
    void status(const QString& message);                          // transient progress ("Loading playlist…")

private:
    void classify(const QString& src, const QString& text, const QString& title); // was handleM3u
    QNetworkAccessManager* nam_ = nullptr; // lazily created for remote playlists
};
```

`StreamResolver.cpp`: bodies move verbatim — the anonymous-namespace helpers (MainWindow.cpp:170-231) become the statics; `openM3u` (2192-2220) becomes `resolve` (`statusBar()->showMessage(...)` → `emit status(...)`; `docNam_` → `nam_`); `handleM3u` (2222-2255) becomes `classify`, with its four exits mapped to the four signals — the IPTV branch's body (stop playback / build queue / RecentStore) does NOT move; `classify` just emits `playQueue(urls, titles, src, title)`. Copy the `logSafeUrl` helper (151-159) and a local `srLog` (pattern from mwLog).

- [ ] **Step 4: Run the probe**

Run: `cmake --build build --config Release --target probe_m3u && ./build/Release/probe_m3u.exe`
Expected: `M3U-OK`, exit 0

- [ ] **Step 5: Rewire MainWindow**

Delete `openM3u`/`handleM3u` and the moved namespace helpers (keep `logSafeUrl` — still used by other MainWindow logging). In the constructor create the resolver and connect:

```cpp
streams_ = new StreamResolver(this);
connect(streams_, &StreamResolver::status, this,
        [this](const QString& m) { statusBar()->showMessage(m); });
connect(streams_, &StreamResolver::playDirect, this,
        [this](const QString& url, const QString& title) { playStream(url, QString(), title); });
connect(streams_, &StreamResolver::openDisc, this,
        [this](const QString& src, const QString& title) { openGamePath(src, title); });
connect(streams_, &StreamResolver::playQueue, this,
        [this](const QStringList& urls, const QStringList& titles,
               const QString& src, const QString& title) {
    // An IPTV / media playlist: build a channel queue (the list panel + next/prev), play the first entry.
    currentNextSourceCapable_ = false;
    retro_->stop(); book_->persist(); pdf_->persist(); comic_->persist();
    setAudioQueue(urls, 0, titles);
    RecentStore::add({ src, title.isEmpty() ? QFileInfo(src).completeBaseName() : title,
                       QStringLiteral("video"), QString(), src });
});
```

Call sites: `openVideoPath:1455` and `openStreamUrl:2161` replace `openM3u(x, t)` with `streams_->resolve(x, t)`; `isM3uRef(...)` call sites use `StreamResolver::isM3uRef(...)`. Verify none remain: `grep -n "openM3u\|handleM3u\|isM3uRef\|parseM3u" native/src/ui/MainWindow.cpp` shows only `StreamResolver::` uses and the connect block.

- [ ] **Step 6: Build + verify live**

Run: `cmake --build build --config Release --target mymediavault probe_m3u`
Expected: clean build, `M3U-OK`.

Live: deploy, launch with `MMV_UITEST=1`. Create `scratchpad/test.m3u` with two entries pointing at any two local audio files (see `C:\MyMediaVault-app` media or any mp3s), then in the app: Settings → (or the stream prompt) — simplest deterministic path is Open Video on the .m3u file via the recent/open flow. Drive with uitest keys; confirm via `uitest.py state` that the player page is active and the queue panel is visible, `shot scratchpad/m3u-queue.png`. Exit playback (back), confirm home state.

- [ ] **Step 7: Commit**

```bash
git add -A native/src/media native/tools/probe_m3u.cpp native/src/ui/MainWindow.h native/src/ui/MainWindow.cpp native/CMakeLists.txt
git commit -m "refactor: extract StreamResolver — m3u/stream classification out of MainWindow, probe-tested"
```

---

### Task 3: PlaybackSession — the audio-queue + resume state machine

**Files:**
- Create: `native/src/media/PlaybackSession.h`, `native/src/media/PlaybackSession.cpp`
- Create: `native/tools/probe_playback.cpp`
- Modify: `native/src/ui/MainWindow.h` — delete members `tracks_`, `trackIndex_`, `resumePath_`, `resumeSeek_`, `audioPos_`, `lastSavedPos_` and method decls `setAudioQueue`, `playTrack`, `clearAudioQueue`, `beginResume`, `persistResume`, `finishResume`; keep `duration_` (the seek bar uses it); add `PlaybackSession* session_`
- Modify: `native/src/ui/MainWindow.cpp` — move `setAudioQueue` (1523-1534), `playTrack` (1536-1545), `nextTrack` (1547-1550), `prevTrack` (1552-1555), the queue-advance part of `onTrackEnded` (1557-1565), `beginResume` (1609-1617), `persistResume` (1619-1630), `finishResume` (1632-1640), `clearAudioQueue` (1642-1651), plus the `store()`/`mediaResumeKey`/`legacyAudiobookKey` statics (234-255 — `store()` stays in MainWindow too; progress sync at `serializeProgress`/`mergeProgress` still uses it)
- Modify: `native/CMakeLists.txt`

**Interfaces:**
- Consumes: nothing from Tasks 1-2 (parallel-safe after Task 1).
- Produces:
  - `explicit PlaybackSession(const QString& settingsFile = QString(), QObject* parent = nullptr)` — empty `settingsFile` = the app store (`AppPaths::dataDir() + "/mymediavault.ini"`); probes pass a temp path
  - `void setQueue(const QStringList& files, int startIndex, const QStringList& titles = {})`
  - `void playIndex(int index)` / `void next()` / `void prev()` / `void handleTrackEnd()` (advances the queue or emits `queueFinished`)
  - `void clearQueue()` — persists then resets (was `clearAudioQueue` minus the widget lines)
  - `void beginResume(const QString& pathOrKey)` / `void persistResume()` / `void finishResume()`
  - `void setPosition(double s)` / `void setDuration(double s)` (fed from mpv callbacks)
  - `double takeResumeSeek()` — returns the pending resume target once, then 0 (consumed by `onDuration`)
  - `int currentIndex() const` / `int count() const` / `QString trackAt(int) const` / `double position() const`
  - signals: `playRequested(QString path)` (host hands it to mpv), `trackChanged(int index, int count, QString displayTitle)`, `queueChanged(QStringList titles, int current)` (host rebuilds `playlist_`), `queueCleared()`, `queueFinished()` (host runs scrobble-stop / next-episode), `resumeSaved()` (host schedules the cloud progress push)

- [ ] **Step 1: Write the failing probe**

`native/tools/probe_playback.cpp`:

```cpp
// Headless test for PlaybackSession: queue advance (next/prev/track-end), resume position
// round-trip through a scratch settings file, and the one-shot resume seek. Prints PLAYBACK-OK.
#include <QCoreApplication>
#include <QTemporaryDir>
#include "../src/media/PlaybackSession.h"

static int fails = 0;
#define CHECK(cond, name) do { if (cond) printf("PASS %s\n", name); \
    else { printf("FAIL %s\n", name); ++fails; } } while (0)

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir tmp;
    const QString ini = tmp.filePath("store.ini");

    PlaybackSession s(ini);
    QStringList played;
    QObject::connect(&s, &PlaybackSession::playRequested,
                     [&](const QString& p) { played << p; });
    int finished = 0;
    QObject::connect(&s, &PlaybackSession::queueFinished, [&] { ++finished; });

    s.setQueue({ "a.mp3", "b.mp3", "c.mp3" }, 1);
    CHECK(played == QStringList{ "b.mp3" }, "setQueue starts at startIndex");
    s.next();  CHECK(played.last() == "c.mp3", "next advances");
    s.prev();  CHECK(played.last() == "b.mp3", "prev steps back");
    s.handleTrackEnd();
    CHECK(played.last() == "c.mp3" && finished == 0, "track end auto-advances");
    s.handleTrackEnd();
    CHECK(finished == 1, "track end at the last track emits queueFinished");

    // Resume round-trip: position persists per file and is consumed once on re-open.
    s.beginResume("X:/book.m4b");
    s.setDuration(3600.0);
    s.setPosition(1234.0);
    s.persistResume();
    PlaybackSession s2(ini);
    s2.beginResume("X:/book.m4b");
    CHECK(qFuzzyCompare(s2.takeResumeSeek(), 1234.0), "resume position survives a new session");
    CHECK(qFuzzyCompare(s2.takeResumeSeek() + 1.0, 1.0), "resume seek is consumed once");
    s2.finishResume();
    PlaybackSession s3(ini);
    s3.beginResume("X:/book.m4b");
    CHECK(qFuzzyCompare(s3.takeResumeSeek() + 1.0, 1.0), "finishResume drops the saved position");

    if (fails == 0) printf("PLAYBACK-OK\n");
    return fails == 0 ? 0 : 1;
}
```

CMake:

```cmake
    # Headless test for PlaybackSession (queue advance + resume persistence).
    add_executable(probe_playback tools/probe_playback.cpp
        src/media/PlaybackSession.cpp src/media/PlaybackSession.h)
    target_include_directories(probe_playback PRIVATE src src/media)
    target_link_libraries(probe_playback PRIVATE Qt6::Core)
```

- [ ] **Step 2: Run it to verify it fails**

Run: `cmake --build build --config Release --target probe_playback`
Expected: FAIL — `PlaybackSession.h: No such file or directory`

- [ ] **Step 3: Implement PlaybackSession**

Header per the Produces block above (QObject; members `tracks_`, `trackIndex_ = -1`, `resumePath_`, `resumeSeek_ = 0.0`, `audioPos_ = 0.0`, `lastSavedPos_ = -100.0`, `duration_ = 0.0`, `QString settingsFile_`; private `QSettings& store()` returning a member `QSettings` built from `settingsFile_`; private statics `mediaResumeKey`/`legacyAudiobookKey` moved verbatim from MainWindow.cpp:244-255). Bodies move verbatim with the widget lines LEFT OUT (they become signals):

- `setQueue` = `setAudioQueue` 1523-1534 minus `playlist_`/`stack_`/`revealMediaControls` lines → after storing the queue emit `queueChanged(displayTitles, startIndex)`, then `playIndex(startIndex)`.
- `playIndex` = `playTrack` 1536-1545: `persistResume(); trackIndex_ = index; beginResume(tracks_[index]);` then `emit trackChanged(index, tracks_.size(), titleAt(index)); emit playRequested(tracks_[index]);` (the `playlist_->setCurrentRow` and `statusBar()` lines are host-side, driven by `trackChanged`).
- `handleTrackEnd` = the queue-advance from `onTrackEnded` 1562: `finishResume(); if (trackIndex_ >= 0 && trackIndex_ + 1 < tracks_.size()) { playIndex(trackIndex_ + 1); return; } emit queueFinished();`
- `persistResume` 1619-1630: the `scheduleProgressSync()` line becomes `emit resumeSaved();`.
- `clearQueue` = `clearAudioQueue` 1642-1651 minus the `playlist_` lines → ends with `emit queueCleared();`.
- `takeResumeSeek`: `const double s = resumeSeek_; resumeSeek_ = 0.0; return s;`

- [ ] **Step 4: Run the probe**

Run: `cmake --build build --config Release --target probe_playback && ./build/Release/probe_playback.exe`
Expected: `PLAYBACK-OK`, exit 0

- [ ] **Step 5: Rewire MainWindow**

Construct `session_ = new PlaybackSession(QString(), this);` early in the ctor, connect:

```cpp
connect(session_, &PlaybackSession::playRequested, this,
        [this](const QString& p) { player_->play(p); });
connect(session_, &PlaybackSession::queueChanged, this,
        [this](const QStringList& titles, int current) {
    playlist_->clear();
    for (const QString& t : titles) playlist_->addItem(t);
    playlist_->setCurrentRow(current);
    playlist_->setVisible(true);
    stack_->setCurrentWidget(playerPage_);
    revealMediaControls();
});
connect(session_, &PlaybackSession::trackChanged, this,
        [this](int i, int n, const QString&) {
    playlist_->setCurrentRow(i);
    statusBar()->showMessage(tr("Track %1 of %2").arg(i + 1).arg(n), 3000);
});
connect(session_, &PlaybackSession::queueCleared, this,
        [this] { if (playlist_) { playlist_->clear(); playlist_->setVisible(false); } });
connect(session_, &PlaybackSession::queueFinished, this, [this] {
    stopScrobble(); // a finished video scrobbles a stop at ~100% -> marked watched
    if (Settings::autoplayNextEpisode()) tryPlayNextEpisode();
});
connect(session_, &PlaybackSession::resumeSaved, this, &MainWindow::scheduleProgressSync);
```

Then the mechanical replacements (verify with `grep -n "tracks_\|trackIndex_\|resumePath_\|resumeSeek_\|audioPos_\|lastSavedPos_\|setAudioQueue\|playTrack\|clearAudioQueue\|beginResume\|persistResume\|finishResume" native/src/ui/MainWindow.cpp` — every hit becomes a `session_->` call or is inside a moved body):

- `setAudioQueue(f, i, t)` → `session_->setQueue(f, i, t)`; `nextTrack()`/`prevTrack()` slots → `session_->next()` / `session_->prev()`; `onTrackEnded()` slot body → `session_->handleTrackEnd();` (scrobble/next-episode now hang off `queueFinished`).
- `beginResume/persistResume/finishResume/clearAudioQueue` → `session_->…` (`clearAudioQueue()` → `session_->clearQueue()`).
- `onPosition(s)`: add `session_->setPosition(s);` (keep the existing seek-bar update); `onDuration(s)`: keep `duration_ = s`, add `session_->setDuration(s);` and replace the resumeSeek-apply block with `const double at = session_->takeResumeSeek(); if (at > 1.0) player_->seek(at);` (match the existing guard/seek call exactly as found in onDuration).
- `openAudioStream` (2257-2278) builds a one-track queue by member access — rewrite those lines as `session_->setQueue({ url }, 0, { t }); session_->beginResume(rkey);` wait — `setQueue` already begins resume for `url`, but the resume key must be `rkey` (the stable id), not the URL. So: `session_->setQueue({ url }, 0, { t });` then `session_->beginResume(rkey);` immediately after (beginResume just re-keys the tracking; playback is already rolling). Same pattern in `playStream` (2178: `beginResume(rkey)` → `session_->beginResume(rkey)`).
- `persistResume()` in `closeEvent`/page-leave call sites → `session_->persistResume()`.
- `serializeProgress`/`mergeProgress` keep using MainWindow's own `store()` (unchanged).

- [ ] **Step 6: Build + verify live**

Run: `cmake --build build --config Release --target mymediavault probe_playback probe_notifier probe_m3u`
Expected: clean build, all three sentinels.

Live: deploy, `MMV_UITEST=1` launch. Drive: open an audio folder (Home → Music/recent audio entry), confirm via `uitest.py state` the player page + queue list; `uitest.py key right` next-track behavior if mapped, else use the transport; play ~30s, back out to Home, re-open the same item, confirm it resumes (state shows position > 0 / screenshot the seek bar: `shot scratchpad/resume-check.png`). Then a video: open, wait, back — reopen resumes.

- [ ] **Step 7: Commit**

```bash
git add -A native/src/media native/tools/probe_playback.cpp native/src/ui/MainWindow.h native/src/ui/MainWindow.cpp native/CMakeLists.txt
git commit -m "refactor: extract PlaybackSession — audio queue + resume state machine, probe-tested"
```

---

### Task 4: GameLauncher — the launch pipeline + external-emulator lifecycle

**Files:**
- Create: `native/src/launch/GameLauncher.h`, `native/src/launch/GameLauncher.cpp`
- Modify: `native/src/ui/MainWindow.h` — delete decls `openGamePath` internals' helpers `ensureEmu`, `launchExternalGame`, `runEmulator`, `startEmuHotkeyWatch`, `stopEmuHotkeyWatch`, `pollEmuExitHotkey`, `beginPlaySession`, `endPlaySession` and members `emu_`, `pendingEmu*`, `emuHotkeyTimer_`, `emuComboPrev_`, `emuEscPrev_`, `emuRunClock_`, `emuDisplayName_`, `emuUserClosing_`, `activePlayId_`, `activePlayStart_`; KEEP `emuPage_`, `emuLabel_`, `emuStopBtn_`, `emuReturnState_`, `ensureEmuPage()`; add `GameLauncher* launcher_`
- Modify: `native/src/ui/MainWindow.cpp` — move `resolveDiscDescriptor` (1668-1698), `openGamePath` (1700-1834), `ensureEmu` (1838-1916), hotkey watch (1923-1975), `launchExternalGame` (1999-2015), `runEmulator` (2017-2058), `beginPlaySession`/`endPlaySession` (grep for their definitions)
- Modify: `native/CMakeLists.txt` (app source list; no probe — this seam is process/hardware-bound, verified live)

**Interfaces:**
- Consumes: `Notifier` (Task 1): launcher errors emit `notifyUser` → connected to `notifier_->notify`. `PlaybackSession` (Task 3): the `aboutToLaunch` handler calls `session_->clearQueue()`.
- Produces:
  - `GameLauncher(RetroView* retro, QObject* parent)` — retro supplies `openGame`, `stop`, `gamepad()`
  - `void open(const QString& rom, const QString& title = {}, const QString& thumb = {}, const QString& key = {}, const QString& systemHint = {})` — the full `openGamePath` pipeline (archive → system → disc descriptor → core/BIOS → retro or external)
  - `struct CorePlan { QString corePath; QString core; QString launchRom; QString systemId; QString error; }; CorePlan prepareCore(const QString& rom, const QString& systemHint)` — the pipeline's resolution half, reused by MainWindow's split-pane branch
  - `void runEmulator(const ExternalEmulator& em, const QString& rom = {}, …same trailing args as today)` / `void install(const ExternalEmulator& em)` / `bool emulatorBusy() const` / `void forceCloseEmulator()` — for the Emulators settings panel + wait-page Stop button
  - signals: `aboutToLaunch()` (host stops player/readers/queue), `showRetroRequested()` (host `stack_->setCurrentWidget(retro_)`), `waitPage(QString text, bool stopVisible)` (host builds/updates the emu wait page + shows it), `waitPageDone()` (host returns Home if the wait page is current), `minimizeRequested()` / `restoreRequested()` (host saves/restores window state), `statusMessage(QString text, int ms)` (status bar), `notifyUser(QString text, int ms)` (→ Notifier)

- [ ] **Step 1: Create GameLauncher and move the pipeline**

Header per the Produces block (members: `RetroView* retro_`, `EmulatorManager* emu_ = nullptr`, the moved `pendingEmu*` strings, hotkey-timer members, `emuRunClock_`, `emuDisplayName_`, `emuUserClosing_`, `activePlayId_`, `activePlayStart_`; local `glLog` copy of mwLog). Bodies move verbatim with these substitutions — no other edits:

| in the moved code | becomes |
|---|---|
| `notify(x, y)` | `emit notifyUser(x, y)` |
| `statusBar()->showMessage(x[, y])` | `emit statusMessage(x, y /*or 0*/)` |
| `stack_->setCurrentWidget(retro_)` | `emit showRetroRequested()` |
| `ensureEmuPage(); emuLabel_->setText(x); emuStopBtn_->setVisible(b); stack_->setCurrentWidget(emuPage_)` | `emit waitPage(x, b)` |
| `if (emuPage_ && stack_->currentWidget() == emuPage_) {…}` (status connect, 1846-1850) | fold into `emit waitPage(line, false)` guarded by `emu_->busy()` |
| `emuReturnState_ = windowState(); showMinimized();` | `emit minimizeRequested()` |
| the `isMinimized()` restore block (1877-1884) | `emit restoreRequested()` |
| `if (stack_->currentWidget() == emuPage_) openHome();` | `emit waitPageDone()` |
| `player_->stop(); book_->persist(); pdf_->persist(); comic_->persist(); clearAudioQueue();` | `emit aboutToLaunch()` |
| split-pane branch inside `openGamePath` (1806-1814) | DELETED here — split stays in MainWindow (Step 2) |
| `retro_->openGame / retro_->stop / retro_->gamepad()` | unchanged (`retro_` is a ctor arg) |

`prepareCore(rom, systemHint)` is carved from `openGamePath`'s middle: system resolution (1735-1753) + disc descriptor (1757-1760) + core ensure (1779-1797) with `CoreManager::ensureCore(core, this, …)` → `CoreManager::ensureCore(core, nullptr, …)` and progress via `emit statusMessage(…, 0)`; `open()` calls it and continues with the retro/external branches. `ensureBios` (1802) stays in `open()` after `prepareCore` succeeds.

- [ ] **Step 2: Rewire MainWindow**

In the ctor (after `session_`):

```cpp
launcher_ = new GameLauncher(retro_, this);
connect(launcher_, &GameLauncher::aboutToLaunch, this, [this] {
    player_->stop(); book_->persist(); pdf_->persist(); comic_->persist();
    session_->clearQueue();
});
connect(launcher_, &GameLauncher::showRetroRequested, this,
        [this] { stack_->setCurrentWidget(retro_); });
connect(launcher_, &GameLauncher::waitPage, this, [this](const QString& text, bool stop) {
    ensureEmuPage();
    emuLabel_->setText(text);
    emuStopBtn_->setVisible(stop);
    stack_->setCurrentWidget(emuPage_);
});
connect(launcher_, &GameLauncher::waitPageDone, this,
        [this] { if (stack_->currentWidget() == emuPage_) openHome(); });
connect(launcher_, &GameLauncher::minimizeRequested, this, [this] {
    emuReturnState_ = windowState();
    showMinimized();
});
connect(launcher_, &GameLauncher::restoreRequested, this, [this] {
    if (!isMinimized()) return; // come back to where we were before handing off
    if (emuReturnState_ & Qt::WindowFullScreen)    showFullScreen();
    else if (emuReturnState_ & Qt::WindowMaximized) showMaximized();
    else                                            showNormal();
    raise();
    activateWindow();
});
connect(launcher_, &GameLauncher::statusMessage, this,
        [this](const QString& m, int ms) { statusBar()->showMessage(m, ms); });
connect(launcher_, &GameLauncher::notifyUser, this,
        [this](const QString& m, int ms) { notifier_->notify(m, ms); });
```

`MainWindow::openGamePath` becomes a thin router that keeps ONLY the split-pane branch:

```cpp
void MainWindow::openGamePath(const QString& rom, const QString& title, const QString& thumb,
                              const QString& key, const QString& systemHint)
{
    if (splitTarget_) // run the ROM in the focused pane's own emulator instead of the full-screen one
    {
        const GameLauncher::CorePlan plan = launcher_->prepareCore(rom, systemHint);
        if (plan.corePath.isEmpty())
        {
            notifier_->notify(plan.error.isEmpty() ? tr("Can't run game.") : plan.error, 7000);
            return;
        }
        const QString recentTitle = title.isEmpty() ? QFileInfo(plan.launchRom).completeBaseName() : title;
        splitTarget_->openGame(plan.corePath, plan.launchRom, plan.core);
        RecentStore::add({ plan.launchRom, recentTitle, QStringLiteral("game"), thumb, key, plan.systemId });
        PlayStats::markPlayed(PlayStats::identity(key, plan.launchRom)); // split panes aren't session-timed
        finishSplitOpen();
        return;
    }
    launcher_->open(rom, title, thumb, key, systemHint);
}
```

The Emulators panel (`openEmulatorManager`, 2060-2124): `ensureEmu(); if (emu_->busy())` → `if (launcher_->emulatorBusy())`; `emu_->install(emCopy)` → `launcher_->install(emCopy)`; `runEmulator(emCopy)` → `launcher_->runEmulator(emCopy)`. The wait page's Stop button connect (1993) → `connect(emuStopBtn_, &QPushButton::clicked, this, [this] { launcher_->forceCloseEmulator(); });`. `launchExternalGame`'s split guard (2009-2013) moves into MainWindow's split branch region — externals can't split, so before delegating: keep the existing message via `statusMessage`. Verify: `grep -n "emu_\|pendingEmu\|emuHotkey\|beginPlaySession\|endPlaySession\|launchExternalGame\|runEmulator\|ensureEmu\b" native/src/ui/MainWindow.cpp` — only `launcher_->` calls, the wait-page widgets, and connects remain.

- [ ] **Step 3: Build + verify live (the touchy one — take it slow)**

Run: `cmake --build build --config Release --target mymediavault`
Expected: clean build.

Live, after deploy with `MMV_UITEST=1`:
1. Libretro path: drive Home → Games → a console with local ROMs → launch a game. `uitest.py state` shows the retro page; `shot scratchpad/launch-retro.png` shows gameplay. Esc → pause/exit back to Home.
2. External path: launch a game on a standalone-emulator system (Dolphin/PCSX2 — whatever is installed). Confirm: wait page appears, MMV minimizes, the emulator opens; press Esc (global hotkey) → the emulator closes and MMV restores. `shot scratchpad/launch-external-return.png`.
3. Failure path: from the stream prompt or a bad file open, confirm errors surface as notices (Notifier), not silence.
4. Confirm play time banked: reopen the game's themed panel — last-played updated.

- [ ] **Step 4: Commit**

```bash
git add -A native/src/launch native/src/ui/MainWindow.h native/src/ui/MainWindow.cpp native/CMakeLists.txt
git commit -m "refactor: extract GameLauncher — launch pipeline, external-emulator lifecycle, exit hotkey, play timing"
```

---

### Task 5: Synthetic browse catalogs — pure builders out of HomeView

**Files:**
- Create: `native/src/browse/SyntheticCatalogs.h`, `native/src/browse/SyntheticCatalogs.cpp`
- Create: `native/tools/probe_browse.cpp`
- Modify: `native/src/ui/HomeView.h` (decls: drop `populateRecents/populateDownloads/populateFavorites` param docs, add `void showSyntheticCatalog(const MediaCatalog& cat);`)
- Modify: `native/src/ui/HomeView.cpp` (`populateRecents` 1674-1703, `populateDownloads` 1730-1757, `populateFavorites` 1772-1794, plus the `iconTypeForKind` helper — find it: `grep -n "iconTypeForKind" native/src/ui/HomeView.cpp`)
- Modify: `native/CMakeLists.txt`

**Interfaces:**
- Consumes: nothing from Tasks 1-4 (parallel-safe).
- Produces (`namespace browse`, in SyntheticCatalogs.h):
  - `QString iconTypeForKind(const QString& kind)` (moved helper)
  - `MediaCatalog recentsCatalog(const QList<RecentItem>& all, const QString& marker)` — marker = `"<kind>"` or `"<kind>|<system>"`; keeps the pcgame-counts-as-game rule
  - `MediaCatalog downloadsCatalog(const QList<DownloadedItem>& all, const QString& marker, const std::function<bool(const QString&)>& fileExists = {})` — default `{}` = `QFileInfo::exists`; the probe injects a fake
  - `MediaCatalog favoritesCatalog(const QList<FavoriteItem>& all, const QString& system)`
  - Builders are PURE: store lists come in as arguments (HomeView passes `RecentStore::list()` etc.), items map with the same offline-first `MetaCache::displayImage` thumbnail rule, titles fall back to the file base name.

- [ ] **Step 1: Write the failing probe**

`native/tools/probe_browse.cpp`:

```cpp
// Headless test for the synthetic browse-catalog builders (Recent / Downloaded / Favorites):
// kind+system filtering, the pcgame-in-games rule, and missing-file hiding. Prints BROWSE-OK.
#include <QCoreApplication>
#include "../src/browse/SyntheticCatalogs.h"

static int fails = 0;
#define CHECK(cond, name) do { if (cond) printf("PASS %s\n", name); \
    else { printf("FAIL %s\n", name); ++fails; } } while (0)

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // NOTE: initialize these structs with the real field names/order from RecentStore.h /
    // DownloadsStore.h / FavoritesStore.h (designated init or field-by-field — check the headers).
    QList<RecentItem> recents;
    { RecentItem r; r.path = "C:/v/movie.mkv"; r.title = "Movie"; r.kind = "video"; recents << r; }
    { RecentItem r; r.path = "C:/g/mario.nes"; r.title = "Mario"; r.kind = "game"; r.system = "nes"; recents << r; }
    { RecentItem r; r.path = "C:/g/doom.exe";  r.title = "Doom";  r.kind = "pcgame"; r.system = "pc"; recents << r; }

    auto vids = browse::recentsCatalog(recents, "video");
    CHECK(vids.items.size() == 1 && vids.items[0].title == "Movie", "recents: kind filter");
    auto games = browse::recentsCatalog(recents, "game");
    CHECK(games.items.size() == 2, "recents: pcgame counts as game");
    auto nes = browse::recentsCatalog(recents, "game|nes");
    CHECK(nes.items.size() == 1 && nes.items[0].title == "Mario", "recents: per-console scope");

    QList<DownloadedItem> dls;
    { DownloadedItem d; d.path = "C:/dl/here.mkv"; d.title = "Here"; d.kind = "video"; dls << d; }
    { DownloadedItem d; d.path = "C:/dl/gone.mkv"; d.title = "Gone"; d.kind = "video"; dls << d; }
    auto have = browse::downloadsCatalog(dls, "video|",
        [](const QString& p) { return p.contains("here"); }); // fake existence check
    CHECK(have.items.size() == 1 && have.items[0].title == "Here",
          "downloads: deleted-outside-the-app entries hidden");

    QList<FavoriteItem> favs;
    { FavoriteItem f; f.path = "C:/g/zelda.sfc"; f.title = "Zelda"; f.system = "snes"; favs << f; }
    { FavoriteItem f; f.path = "";               f.title = "NoPath"; favs << f; } // streamed fav: no console home
    auto snes = browse::favoritesCatalog(favs, "snes");
    CHECK(snes.items.size() == 1 && snes.items[0].title == "Zelda", "favorites: system scope + path-only");

    if (fails == 0) printf("BROWSE-OK\n");
    return fails == 0 ? 0 : 1;
}
```

CMake (link what the builders pull in — MediaItem/MetaCache and the three store headers; mirror probe_gamelist's pattern at CMakeLists:242-245):

```cmake
    # Headless test for the synthetic browse-catalog builders (Recent/Downloaded/Favorites).
    add_executable(probe_browse tools/probe_browse.cpp
        src/browse/SyntheticCatalogs.cpp src/browse/SyntheticCatalogs.h
        src/core/MetaCache.cpp src/addons/AddonModels.cpp)
    target_include_directories(probe_browse PRIVATE src src/core src/addons src/browse)
    target_link_libraries(probe_browse PRIVATE Qt6::Core Qt6::Network Qt6::Gui)
```

(If MetaCache drags in more deps, add those .cpp files the same way the other probes do — the linker errors name them.)

- [ ] **Step 2: Run it to verify it fails**

Run: `cmake --build build --config Release --target probe_browse`
Expected: FAIL — `SyntheticCatalogs.h: No such file or directory`

- [ ] **Step 3: Implement the builders**

`SyntheticCatalogs.h` declares the four functions (Produces block). `SyntheticCatalogs.cpp` bodies are the loops moved verbatim from HomeView.cpp (1680-1698, 1736-1751, 1774-1788) with `RecentStore::list()` → the `all` parameter, `QFileInfo::exists(d.path)` → `fileExists ? fileExists(d.path) : QFileInfo::exists(d.path)`, and `tr("Recent")` → `QObject::tr("Recent")` (same for "Downloaded"/"Favorites"). `iconTypeForKind` moves in unchanged.

- [ ] **Step 4: Run the probe**

Run: `cmake --build build --config Release --target probe_browse && ./build/Release/probe_browse.exe`
Expected: `BROWSE-OK`, exit 0

- [ ] **Step 5: Rewire HomeView**

Add the shared boilerplate method (this is the duplication the three populates carried):

```cpp
// Show a locally built (addon-less) catalog level: reset paging state and hand it to the grid.
void HomeView::showSyntheticCatalog(const MediaCatalog& cat)
{
    pendingReqId_ = -1; loading_ = false; hasMore_ = false; currentPage_ = 1;
    hideMeta();
    if (carouselMode_ || xmbMode_) grid_->hide(); else grid_->show();
    populate(cat, /*append*/ false);
}
```

The three populates collapse to one line each:

```cpp
void HomeView::populateRecents(const QString& marker)
{ showSyntheticCatalog(browse::recentsCatalog(RecentStore::list(), marker)); }

void HomeView::populateDownloads(const QString& marker)
{ showSyntheticCatalog(browse::downloadsCatalog(DownloadsStore::list(), marker)); }

void HomeView::populateFavorites(const QString& system)
{ showSyntheticCatalog(browse::favoritesCatalog(FavoritesStore::list(), system)); }
```

Delete HomeView's local `iconTypeForKind` and include `../browse/SyntheticCatalogs.h`; fix its other call sites (grep from Task file list) to `browse::iconTypeForKind`. Add both SyntheticCatalogs files to the `mymediavault` source list.

- [ ] **Step 6: Build + verify live**

Run: `cmake --build build --config Release --target mymediavault probe_browse`
Expected: clean build, `BROWSE-OK`.

Live: deploy, `MMV_UITEST=1`. Drive into a games console → Recent / Downloaded / Favorites folders each render their rows exactly as before (state + `shot scratchpad/synthetic-levels.png`); open one recent entry to confirm activation still routes. Check a non-game catalogue's Recent too (kind filter without system).

- [ ] **Step 7: Commit**

```bash
git add -A native/src/browse native/tools/probe_browse.cpp native/src/ui/HomeView.h native/src/ui/HomeView.cpp native/CMakeLists.txt
git commit -m "refactor: extract synthetic browse catalogs (Recent/Downloaded/Favorites) as pure probe-tested builders"
```

---

### Task 6: Full verification sweep + close out plan 1

**Files:**
- Modify: `docs/superpowers/specs/2026-07-16-foundation-refactor-design.md` (status line)
- Modify: `native/tools/run-headless-probes.sh` (register the four new probes so CI runs them)

- [ ] **Step 1: Register the new probes in CI's runner**

In `run-headless-probes.sh`, after the existing `run` invocations, add (matching the script's `run <name> <sentinel> <exe>` convention and `findexe` lookup):

```bash
for p in "probe_notifier NOTIFIER-OK" "probe_m3u M3U-OK" "probe_playback PLAYBACK-OK" "probe_browse BROWSE-OK"; do
  set -- $p
  if exe=$(findexe "$1"); then run "$1" "$2" "$exe"; else echo "SKIP: $1 (not built)"; fi
done
```

- [ ] **Step 2: Run everything**

Run: `cmake --build build --config Release && BUILD_DIR=build bash native/tools/run-headless-probes.sh && ./build/Release/probe_nav.exe`
Expected: every probe PASS, no FAIL lines, exit 0.

- [ ] **Step 3: Full live smoke**

Deploy, `MMV_UITEST=1`, one pass through every extracted seam: video open+resume, audio queue next/prev, an .m3u queue, a libretro game launch+exit, an external emulator launch+hotkey-return, Recent/Downloaded/Favorites folders, and one visible error notice. Screenshot each to `scratchpad/`. Line counts as a sanity metric: `wc -l native/src/ui/MainWindow.cpp native/src/ui/HomeView.cpp` — expect MainWindow well under 5,000 and shrinking per the spec trajectory (the ~800 target lands after plan 2's remaining moves).

- [ ] **Step 4: Update the spec status + commit**

Change the spec's `**Status:**` line to `Plan 1 implemented (MainWindow seams + synthetic catalogs); plan 2 pending (async browse providers + sweep)`.

```bash
git add docs/superpowers/specs/2026-07-16-foundation-refactor-design.md native/tools/run-headless-probes.sh
git commit -m "refactor: wire the new seam probes into CI; mark phase-1 plan 1 done"
```
