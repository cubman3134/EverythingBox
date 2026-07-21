# Form-Factor Adaptivity Phase 1 (TV + Mobile on desktop) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the themed UI form-factor adaptive — TV (10-foot) and Mobile (touch) modes with auto-detect + override — entirely built and verified on the desktop.

**Architecture:** One `FormFactor` singleton (mode + derived tokens) threads through the theme engine as a `form` QML context property (3 registration sites) and a C++ accessor for the widget surfaces (OSK/NavOverlay/player chrome/readers/RetroView). Touch enters ONLY through the Nav Contract — taps call the same `nav.select`/`nav.activate` the existing MouseAreas already use; gestures map to existing APIs (`goBack`, `zoomDelta`, `nextPage`). Spec: `docs/superpowers/specs/2026-07-21-formfactor-adaptivity-design.md`.

**Tech Stack:** Qt 6.8.3 MSVC2022, QML (software backend), QWidget gesture framework (readers/pad), QWindowSystemInterface synthetic touch (uitest), headless probes.

## Global Constraints

- Branch: `formfactor/d1-plan` off main. Commit per step-group; NEVER push until the merge gate.
- ANCHOR ON FUNCTION NAMES — line numbers below were scouted at main@387684a and WILL drift.
- Input authority: NOTHING may write selection state or activate outside NavGraph (`select`/`activate`/`move`/`back`). The CI grep (`run-headless-probes.sh`, no-direct-selection-writes) polices QML; keep new QML compliant.
- Setter/data-op fidelity: mode storage via `Settings` accessors only; no behavior change in Desktop mode (all tokens identity — every diff must be a no-op at Desktop defaults).
- Probe env recipe: PATH prepend `/c/Qt/6.8.3/msvc2022_64/bin` and `/c/mpv-dev`; `QT_QPA_PLATFORM=offscreen`; `QT_PLUGIN_PATH=C:\Qt\6.8.3\msvc2022_64\plugins`; `BUILD_DIR=C:/Users/cubma/Project Goliath/build`; runner `native/tools/run-headless-probes.sh`.
- New probes/greps must be registered in `native/tools/run-headless-probes.sh` AND built in `.github/workflows/ci.yml` (target list at the `cmake --build` line, ~:52) — a probe CI doesn't build silently never runs.
- Live verification: deploy Release over `C:\MyMediaVault-app\MyMediaVault.exe` (keep name); `MMV_UITEST=1`; drive ONLY via `python native/tools/uitest.py` — never SendKeys/focus. Protect user data: Weekend Picks playlist untouched; byte-identical `mymediavault.ini` restores.
- Token table (the single source of truth; Desktop row MUST be identity):

| Mode | uiScale | minHitPx (logical) | safeAreaFrac | density |
|---|---|---|---|---|
| Desktop | 1.0 | 0 | 0.0 | 1.0 |
| Tv | 1.3 | 0 | 0.05 | 1.15 |
| Mobile | 1.15 | 44 | 0.0 | 1.1 |

---

### Task 1: FormFactor core + Settings accessors + probe_formfactor

**Files:**
- Create: `native/src/theme2/FormFactor.h`, `native/src/theme2/FormFactor.cpp`
- Modify: `native/src/core/Settings.h`, `native/src/core/Settings.cpp` (follow the `startFullscreen()`/`setStartFullscreen()` accessor pattern at `Settings.h:46-47`)
- Create: `native/tools/probe_formfactor.cpp`
- Modify: `native/CMakeLists.txt` (new probe target — NO Quick dependency: FormFactor is QtCore-only so the probe links lean; place beside the other desktop-only probes), `native/tools/run-headless-probes.sh` (probe list array ~:113), `.github/workflows/ci.yml` (build-target list ~:52)

**Interfaces:**
- Produces (everything later tasks consume):

```cpp
// native/src/theme2/FormFactor.h  (QtCore only — no Quick/Widgets includes)
#pragma once
#include <QObject>

// The ONE form-factor authority (spec: subsystem D §1). Mode resolves from the stored
// "display/mode" setting ("auto"|"desktop"|"tv"|"mobile"); auto => platform detection
// (desktop builds resolve Desktop in Phase 1; Android branches land in Phase 2).
// Tokens derive from mode via the spec table. Desktop tokens are IDENTITY by contract:
// every consumer multiplied/inset by these must be a pixel-for-pixel no-op in Desktop.
class FormFactor : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString mode READ modeName NOTIFY changed)
    Q_PROPERTY(qreal uiScale READ uiScale NOTIFY changed)
    Q_PROPERTY(int minHitPx READ minHitPx NOTIFY changed)
    Q_PROPERTY(qreal safeAreaFrac READ safeAreaFrac NOTIFY changed)
    Q_PROPERTY(qreal density READ density NOTIFY changed)
public:
    enum class Mode { Desktop, Tv, Mobile };
    static FormFactor& instance();

    Mode    mode() const { return mode_; }
    QString modeName() const;      // "desktop" | "tv" | "mobile"
    qreal   uiScale() const;       // table
    int     minHitPx() const;      // table (logical px; consumers already ride Qt DPR)
    qreal   safeAreaFrac() const;  // table
    qreal   density() const;       // table

    void    refresh();             // re-read Settings::displayMode(), re-resolve, emit changed() if different
    static Mode resolveAuto();     // Phase 1: always Desktop on desktop platforms
signals:
    void changed();
private:
    FormFactor();
    Mode mode_ = Mode::Desktop;
};
```

```cpp
// Settings.h additions (namespace fns, exact names later tasks call):
QString displayMode();                    // "auto"|"desktop"|"tv"|"mobile"; default "auto"; key "display/mode"
void    setDisplayMode(const QString&);   // writes + sync; caller then calls FormFactor::instance().refresh()
bool    tvPromptDone();                   // key "display/tvPromptDone", default false
void    setTvPromptDone(bool);
```

- [ ] **Step 1: Write the failing probe.** `native/tools/probe_formfactor.cpp` — a `QCoreApplication` + `CHECK` macro (copy the macro shape from `probe_navqml.cpp` top). Point `AppPaths` at a temp dir (set the same env/mechanism `probe_addon.cpp` uses for an isolated ini — grep `probe_addon.cpp` for how it isolates the store; replicate). Asserts:

```cpp
// 1. Defaults: stored mode absent -> auto -> Desktop identity tokens.
Settings::setDisplayMode(QStringLiteral("auto"));
FormFactor& ff = FormFactor::instance(); ff.refresh();
CHECK(ff.mode() == FormFactor::Mode::Desktop);
CHECK(qFuzzyCompare(ff.uiScale(), 1.0)); CHECK(ff.minHitPx() == 0);
CHECK(qFuzzyCompare(ff.safeAreaFrac() + 1.0, 1.0)); CHECK(qFuzzyCompare(ff.density(), 1.0));
// 2. Explicit override beats auto; full token table per mode (tv: 1.3/0/0.05/1.15; mobile: 1.15/44/0.0/1.1).
// 3. changed() emitted exactly once per real change, not on same-mode refresh (QSignalSpy).
// 4. Unknown stored string falls back to auto/Desktop (corrupt-ini safety).
puts("FORMFACTOR-OK");
```

- [ ] **Step 2: CMake target + run to verify it FAILS to build** (FormFactor doesn't exist yet). Add to `native/CMakeLists.txt` beside the other `if(NOT ANDROID)` probes: `probe_formfactor` sources = `tools/probe_formfactor.cpp src/theme2/FormFactor.cpp src/core/Settings.cpp` + whatever `Settings.cpp` links (check its includes; AppPaths is header-only). Link `Qt6::Core Qt6::Test` (QSignalSpy). Run: `cmake --build build --config Release --target probe_formfactor` → expect compile FAIL (missing FormFactor.h).
- [ ] **Step 3: Implement FormFactor + Settings accessors** per the interfaces above. Token switch in one place. `instance()` = function-local static. `refresh()` compares old/new mode and emits only on change.
- [ ] **Step 4: Build + run the probe.** `cmake --build build --config Release --target probe_formfactor` then (env recipe) `./build/Release/probe_formfactor.exe` → expect `FORMFACTOR-OK`, exit 0.
- [ ] **Step 5: Register** in `run-headless-probes.sh` probe array (`"probe_formfactor FORMFACTOR-OK"`) and add `probe_formfactor` to the ci.yml build-target list. Run the full suite (env recipe) → `ALL HEADLESS PROBES PASSED`.
- [ ] **Step 6: Commit** `feat: FormFactor core — mode + tokens + display/mode setting (D1 Task 1)`.

---

### Task 2: `form` in QML + TV scale/insets on the QML surfaces

**Files:**
- Modify: `native/src/theme2/ThemeEngine.cpp` (`buildView`, context props before `setSource` ~:310-313), `native/src/theme2/ThemedPanelHost.cpp` (`buildView` ~:94-96), `native/src/theme2/ReaderChromeHost.cpp` (inside the `makeStrip` lambda in `buildStrips` ~:173-182 — it runs TWICE, both strips must see `form`; also `layoutStrips` ~:190-200 for C++-side strip heights)
- Modify: `native/src/theme2/qml/ThemeView.qml` (root/content ~:7,:479), `native/src/theme2/qml/elements/Text.qml` (~:15), `native/src/theme2/qml/elements/SettingsPanel.qml` (header ~:88, ListView margins ~:124-126, delegate height ~:146, font literals ~:102,:113,:183), `native/src/theme2/qml/elements/ReaderChrome.qml` (font/size literals)
- Modify: `native/tools/probe_navqml.cpp` (fixtures register `nav` at ~:123,:730,:765 — add `form` beside each; new §19)

**Interfaces:**
- Consumes: `FormFactor::instance()` (Task 1).
- Produces: QML global `form` (`form.mode`, `form.uiScale`, `form.minHitPx`, `form.safeAreaFrac`, `form.density`) available on EVERY themed surface; QML convention later tasks reuse: `readonly property real ffs: (typeof form !== "undefined" && form) ? form.uiScale : 1` on each root (the `typeof` guard keeps any QML loaded without the property working — probe fixtures included).

- [ ] **Step 1: Extend probe_navqml first (RED).** New §19 in `probe_navqml.cpp`: register `form` (= `&FormFactor::instance()`) on the ThemeView fixture; force tv mode via `Settings::setDisplayMode("tv")` + `refresh()`; assert on the loaded root item: `content` inset == `root.width * 0.05` (findChild by objectName — give the ThemeView content Item `objectName: "ffContent"`), and a themed Text's `font.pixelSize` == `round(fraction * host.height * 1.3)` within ±1px. Then Desktop mode → inset 0 and pixelSize identical to the pre-change baseline (identity proof). Run → FAIL (props not registered, no insets).
- [ ] **Step 2: Register `form` at the 3 sites + probe fixtures.** One line each, before `setSource`: `ctx->setContextProperty(QStringLiteral("form"), &FormFactor::instance());`
- [ ] **Step 3: QML scale + insets.**
  - `ThemeView.qml` content Item: `anchors.margins: Math.round(Math.min(root.width, root.height) * ((typeof form !== "undefined" && form) ? form.safeAreaFrac : 0))` + `objectName: "ffContent"`. Backgrounds stay full-bleed (art must reach the edges; only CONTENT is inset).
  - `Text.qml:15`: multiply the existing `fontSize * host.height` product by the root's `ffs`.
  - `SettingsPanel.qml`: root gets `ffs` + a density property; multiply header height 74, delegate heights 56/40/320, the font literals 16/26/17, and ListView margins; add safe-area via root anchors margins (same formula). Row height uses `density`, fonts use `ffs`.
  - `ReaderChrome.qml`: multiply its size/font literals by `ffs`.
  - `ReaderChromeHost::layoutStrips`: multiply `barH`/`botH`/`tocH` by `uiScale()` and inset strip geometry by `safeAreaFrac()` (C++-side — the strips are positioned by `setGeometry`).
- [ ] **Step 4: Probe suite green** (env recipe, full runner). §19 passes; ALL prior sections still pass (the Desktop identity assert is the regression net).
- [ ] **Step 5: Live TV-mode spot-check.** Deploy; seed `display/mode=tv` in the ini (snapshot first); walk home (XMB + Grid + Lumen), a settings panel, a reader — screenshots `d1t2-*`; verify insets + larger type visually; restore ini byte-identical. Expect NO behavioral diffs (selection, activation) — sizing only.
- [ ] **Step 6: Commit** `feat: form context property + TV scale/insets on the QML surfaces (D1 Task 2)`.

---

### Task 3: Widget surfaces + Display-mode row + auto/TV prompt

**Files:**
- Modify: `native/src/ui/nav/NavOverlay.cpp` (stylesheet font sizes ~:49-58, panel width bounds ~:227,:241-245), `native/src/ui/nav/Osk.cpp` (key `setFixedSize(46,40)` ~:48, action-key width ~:78-81, `font-size:15px`)
- Modify: `native/src/ui/MainWindow.cpp`: player chrome build (`mediaControls_` ~:560-599 — transport buttons, `seek_`, `videoBack_`), `openAppearance` themed branch (new Choice row), new `maybeOfferTvMode()` called once post-show
- Modify: `native/src/ui/MediaPane.cpp` (split-pane bar controls minHit in Mobile)
- Modify: `native/src/main.cpp` (call `maybeOfferTvMode` is MainWindow-side; main.cpp itself unchanged unless the startup hook needs it — prefer a `QTimer::singleShot(0,...)` inside MainWindow after show)

**Interfaces:**
- Consumes: `FormFactor::instance()` tokens + `changed()`; `Settings::displayMode/setDisplayMode/tvPromptDone/setTvPromptDone` (Task 1); the Appearance panel builder from B2 Task 6.75 (`openAppearance` themed branch — follow its Choice-row + apply-on-select pattern exactly).
- Produces: `MainWindow::applyFormFactorWidgets()` — one function that re-derives every widget-side size from tokens; connected to `FormFactor::changed`. Later tasks may extend it but not bypass it.

- [ ] **Step 1: Widget sizing through one chokepoint.** Add `applyFormFactorWidgets()`: scales NavOverlay/Osk stylesheet font sizes and Osk key sizes (`qMax(int(46*s), hit)` × `qMax(int(40*s), hit)` where `hit = minHitPx()`), player-chrome button minimum sizes (`setMinimumSize(hit, hit)` when `hit>0`, reset otherwise) + seek slider height, MediaPane bar controls. NavOverlay/Osk take the values via small setters or re-style calls — do NOT duplicate the token math outside this function. Connect `FormFactor::changed → applyFormFactorWidgets` + call once at startup. Desktop identity: with defaults the function must produce today's exact sizes (46/40/14px/16px/15px...).
- [ ] **Step 2: "Display mode" Choice row in Appearance.** In `openAppearance`'s themed row builder: Choice `appr.dispmode`, options Auto/Desktop/TV/Mobile, current from `Settings::displayMode()`; onActivate maps display string → stored value, `Settings::setDisplayMode(v)`, `FormFactor::instance().refresh()` — the `changed` signal then live-restyles (panel restyle rides the SAME `setStyle(settingsPanelStyle())` mechanism as theme apply-on-select — call it from the row handler like the theme row does; QML property bindings on `form.*` update automatically since FormFactor is a live QObject). Classic Appearance panel: UNCHANGED (no new classic UI).
- [ ] **Step 3: One-time TV suggest.** `maybeOfferTvMode()`: only when `Settings::displayMode()=="auto"` && `!Settings::tvPromptDone()` && `isFullScreen()` && primary screen physical width ≥ 700mm (`QGuiApplication::primaryScreen()->physicalSize()` — if physicalSize is empty/zero, DON'T prompt; unreliable EDIDs must fail silent). Prompt via `NavConfirm::ask` Cancel-focused ({"Not now","Use TV mode"}, focusIndex 0, cancelIndex 0); either answer sets `setTvPromptDone(true)`; "Use TV mode" → `setDisplayMode("tv")` + refresh. Fire once via `QTimer::singleShot(0, ...)` after the window shows (post-startup-picker: guard on no modal up — reuse the pattern `promptStartupProfile` sequencing uses).
- [ ] **Step 4: Probe leg.** `probe_formfactor` addition: instantiate an `Osk`-free check is impossible headlessly for widgets? It is possible: probes link Widgets already elsewhere — but keep it lean: assert the pure math helper instead. Extract the size derivation into `FormFactor` helpers (`int hitClamp(int basePx) const` → `qMax(int(basePx*uiScale()), minHitPx())`) and assert the table through them (`hitClamp(46)`: desktop 46, tv 59, mobile 46→44? NO — mobile: `qMax(int(46*1.15)=52, 44)=52`; tv `qMax(59,0)=59`). Use `hitClamp` in `applyFormFactorWidgets` so the probe pins the real math.
- [ ] **Step 5: Build + suite green; live check.** Deploy. Live: Appearance → Display mode TV → OSK/Esc menu visibly larger (screenshots `d1t3-*`); back to Auto → byte-identical sizes; the TV prompt: force with a seeded ini (`display/mode=auto`, `tvPromptDone` absent) + fullscreen — verify prompt appears once, Cancel-focused, and NEVER re-appears after either answer (relaunch to prove). Restore ini byte-identical.
- [ ] **Step 6: Commit** `feat: widget-surface form-factor sizing + Display-mode row + one-time TV suggest (D1 Task 3)`.

---

### Task 4: Touch navigation + uitest touch verb + translation probe

**Files:**
- Modify: `native/src/core/UiTestServer.h` (Hooks struct ~:24-30: add `std::function<void(const QString&)> touch;`), `native/src/core/UiTestServer.cpp` (`handle()` ~:54: new `touch` verb), `native/tools/uitest.py` (client verb)
- Modify: `native/src/ui/MainWindow.cpp` (`updateUiTestServer` ~:1340: implement `h.touch` via `QWindowSystemInterface::handleTouchEvent`)
- Modify: `native/src/theme2/qml/ThemeView.qml` (`gotoItem` ~:207-226 mobile tap semantics; edge-swipe Back; mobile Back affordance), `native/src/theme2/qml/elements/SettingsPanel.qml` (ListView `interactive` in mobile ~:129)
- Modify: `native/tools/probe_navqml.cpp` (new §20: synthetic-touch translation)

**Interfaces:**
- Consumes: `form` QML property (Task 2); `NavGraph::select(zone,index)/activate()/back()` (`NavGraph.h:95-122`); `MainWindow::goBack()` (~:1121).
- Produces: uitest verbs — `touch tap X Y`, `touch flick X1 Y1 X2 Y2 [MS]` (default 150ms, ≥6 interpolated move points), `touch pinch CX CY SCALE [MS]` (two synthetic fingers). App-side they synthesize REAL `QWindowSystemInterface::handleTouchEvent` sequences on the top-level window with a static `QPointingDevice` (TouchScreen type) — real hit-testing, no shortcut into the graph (the whole point is testing the QML handlers).

- [ ] **Step 1: Probe §20 first (RED).** ThemeView fixture at mobile mode (`setDisplayMode("mobile")`+refresh): `QTest::touchEvent` tap on a non-selected grid item → assert graph selection MOVED to it AND `activated` fired (one-tap activate, the mobile semantics); tap on SettingsPanel fixture row → select+activate (already one-click; assert unchanged); flick vertically on the panel ListView → assert `contentY` changed AND selection did NOT; edge-swipe from x<12 rightward ≥80px → assert `backInvoked` on the graph. Desktop mode: tap keeps today's two-step (`select` first, second tap activates) — assert. Run → FAIL.
- [ ] **Step 2: Mobile tap semantics.** `ThemeView.qml` `gotoItem(i)`: current = select-if-different / activate-if-same. New: `if (form-mobile) { host.select(i); nav.activate(); } else { existing two-step }` — implemented INSIDE `gotoItem` (all element MouseAreas route through it already, per the scout inventory; Xmb `gotoCat` stays select-only — category hops shouldn't activate). SettingsPanel rows already one-tap; leave.
- [ ] **Step 3: Flick + edge-swipe + affordance.**
  - SettingsPanel ListView: `interactive: (typeof form !== "undefined" && form) ? form.mode === "mobile" : false` — native kinetic contentY; keep `positionViewAtIndex` on selection change (key nav still snaps); ROW TAP commits selection so flick-then-tap is coherent. Guard: `interactive` ListView + `positionViewAtIndex` fight only during a drag — set `interactive` false is default so Desktop/TV untouched.
  - Edge-swipe: a thin (12px) left-edge `MouseArea` (`drag` threshold 80px horizontal) on ThemeView root + SettingsPanel root, mobile-only (`enabled: form-mobile`), firing `nav.back()`. It must NOT eat taps: propagate presses that don't become drags (`propagateComposedEvents`).
  - Mobile Back affordance: a small chevron button top-left on ThemeView (mobile only, hidden on the root home level — bind to whatever exposes level depth; if none exists, show always on browse/detail views via `currentView !== "home"`), tap → `nav.back()`. SettingsPanel already has a Back header (`:104-107`) — touch-sized by Task 2's scaling.
  - Theme-element flick: INVESTIGATE each element (Grid/Carousel/Channels — are they ListView/GridView/PathView or Repeaters?). Where the element is a native view type: enable `interactive` in mobile like the panel. Where it's a positioned Repeater (likely Xmb): tap-to-move + wheel remain; RECORD as a follow-up in the report — do NOT build custom kinetic physics in this task.
- [ ] **Step 4: uitest touch verb.** Server: parse `touch tap|flick|pinch` args, call `hooks_.touch(arg)`. MainWindow hook: build `QList<QWindowSystemInterface::TouchPoint>` sequences against `windowHandle()` with a `static QPointingDevice* uitestTouch = QPointingDevice::primaryPointingDevice()`-style registered touchscreen device (create via the `QPointingDevice` ctor, register once). tap = press+release same point (2 frames, 30ms apart via a small state machine on a QTimer — do NOT block the pipe handler); flick = press, N interpolated moves over MS, release; pinch = two points diverging by SCALE. Client (`uitest.py`): `touch` subcommand passing the raw line.
- [ ] **Step 5: Probe + suite green; live.** §20 passes; full suite green. Live (deploy, `display/mode=mobile` seeded, snapshot ini): `uitest.py touch tap` a home tile → detail opens (one tap); flick a settings panel → scrolls without losing cursor row highlight; edge-swipe → back navigation; screenshots `d1t4-*`; restore ini.
- [ ] **Step 6: Commit** `feat: touch nav on the contract — mobile tap/flick/edge-back + uitest touch verb (D1 Task 4)`.

---

### Task 5: Player + reader touch

**Files:**
- Modify: `native/src/ui/MainWindow.cpp` (player chrome: the `eventFilter` that reveals controls ~:950 `revealMediaControls`, `controlsHideTimer_` ~:781-796; double-tap seek on `player_`)
- Modify: `native/src/theme2/ReaderChromeHost.cpp` (the reader event filter installed at ~:156 — tap zones + swipe + chrome toggle live HERE so all three readers get one implementation; pinch via `grabGesture`)
- Modify: `native/src/comic/ComicView.*` / `native/src/pdf/PdfView.*` only if `grabGesture`/`event()` must live on the widget itself (prefer the host filter; readers stay untouched if the filter can consume gesture events — verify: `QGestureEvent` goes to the grabbing widget, so `grabGesture(Qt::PinchGesture)` must be called ON the reader widget by the host, and the HOST filter sees it via the filter — confirm with a spike, else add a tiny `event()` override forwarding to the host)

**Interfaces:**
- Consumes: `HostedReader` (`native/src/theme2/HostedReader.h`): `nextPage()/prevPage()` `:25-26`, `zoomDelta(int)` `:34`; `ReaderChromeHost` chrome show/hide (`armAutoHide`/`hideChrome` ~:140); the existing relative-seek path the arrow keys use in the video player (grep `seekRelative|seek(` in MainWindow player handling — reuse EXACTLY that call for double-tap ±10s).
- Produces: player/readers finger-operable; no new public API.

- [ ] **Step 1: Player touch.** In the player chrome eventFilter path: `TouchBegin`/tap on `player_` TOGGLES chrome (today mouse-move reveals; touch tap = toggle — a shown chrome + tap hides, hidden + tap shows and re-arms `controlsHideTimer_`). Double-tap (two taps <350ms) on left/right third = seek −10s/+10s via the existing relative-seek call; show a transient `notify`-style flash (reuse the notify toast). Seek-bar drag + transport taps need nothing new (QSlider/QPushButton handle synthesized-from-touch mouse) — but verify hit sizes ride Task 3's `minHitPx`.
- [ ] **Step 2: Reader tap zones + swipe + pinch** in `ReaderChromeHost`'s filter: tap x<⅓ → `prevPage()`, x>⅔ → `nextPage()`, center → chrome toggle; horizontal touch swipe ≥80px → next/prev (leftward = next, matching page-flip convention); `grabGesture(Qt::PinchGesture)` on the hosted reader when it reports zoom support (comic/pdf — `zoomDelta` exists; ebook gets font size later ONLY IF trivial, else skip: spec scopes pinch to comic/PDF), pinch scale accumulates → `zoomDelta(±1)` steps per 15% scale change. Gate ALL of it on touch events only (mouse untouched — desktop click behavior must not change; tap zones from a MOUSE would break text selection/etc.). Mobile-mode-independent: touch gestures work in any mode (a touchscreen desktop is legit); only AFFORDANCE sizing keys on mode.
- [ ] **Step 3: Verification.** No headless probe (gesture events on live widgets — the uitest layer IS the harness): live with `uitest.py touch` — video: tap toggle, double-tap skip both directions (state verb shows position), drag the seek bar; comic: tap zones page, swipe pages, pinch zooms (state/screenshot evidence); pdf: same spot-check; ebook: tap zones + swipe. Screenshots `d1t5-*`. Desktop mouse regression check: click behavior in the player + readers unchanged (drive the old mouse paths once).
- [ ] **Step 4: Commit** `feat: player + reader touch — tap chrome, double-tap seek, tap-zones/swipe/pinch (D1 Task 5)`.

---

### Task 6: Virtual gamepad

**Files:**
- Create: `native/src/emu/VirtualPad.h`, `native/src/emu/VirtualPad.cpp` (a child QWidget overlay)
- Modify: `native/src/emu/RetroView.h` (~:176 `pressedKeys_` area: add `quint32 virtualPad_ = 0;` + setter), `native/src/emu/RetroView.cpp` (`resolveInput` ~:1650-1662 OR the mask in; `pollInput` ~:885 snapshot `snapBtn_` includes it; `setInputActive(false)` ~:1681 clears it; `buildMenu()` gains a "Virtual pad" toggle row), `native/CMakeLists.txt` (new sources)
- Modify: `native/src/core/Settings.h/.cpp` (`virtualPadOpacity()` 0-100 default 45, `virtualPadEnabled()` default: auto = Mobile mode)

**Interfaces:**
- Consumes: `FormFactor` mode; `Keymap` retro ids (`RETRO_DEVICE_ID_JOYPAD_*` — the ids `resolveInput` matches).
- Produces: `VirtualPad : QWidget` — `WA_AcceptTouchEvents`, multi-touch → a held bitmask (bit per RETRO_DEVICE_ID_JOYPAD id), `maskChanged(quint32)` signal; `RetroView::setVirtualPadMask(quint32)`.

- [ ] **Step 1: Injection first (testable seam).** `RetroView`: `void setVirtualPadMask(quint32 m)` sets `virtualPad_`; in `resolveInput` JOYPAD branch OR in `((virtualPad_ >> id) & 1)` beside `kb || pad_.button(...)`; in `pollInput` the snapshot builds from the same three sources (threaded/split cores read `snapBtn_` — MUST include the mask or the pad is dead exactly on TV-class threaded cores); `setInputActive(false)` zeroes `virtualPad_`. Headless assert: `probe_core` (if it exercises `resolveInput` — check) or a targeted addition wherever RetroView input is probe-testable; if RetroView is not headless-probeable (likely — GL/audio), assert via live only and say so in the report.
- [ ] **Step 2: The overlay.** `VirtualPad`: paints D-pad (4 hit zones), A/B/X/Y cluster, L/R shoulders, Start/Select — flat translucent circles/rounded rects (`opacity` from Settings), layout anchored bottom-left (dpad) / bottom-right (ABXY) / top edges (L/R), all zones ≥ `minHitPx`. Touch: track every `QTouchEvent` point; a point inside a zone holds its bit; D-pad supports diagonal (two bits) and slide-between-directions; release clears. Emit `maskChanged` on change → `setVirtualPadMask`. Mouse presses also work (single-button, for desktop testing).
- [ ] **Step 3: Show/hide.** Parent = RetroView, resized with it. Visible when: `virtualPadEnabled()` explicitly on, OR (auto + Mobile mode). Toggle row in RetroView's existing Esc menu (`buildMenu()` pattern) + opacity slider row there (`kUnbound` — reuse whatever row/slider mechanism the menu already has; if it's buttons-only, opacity cycles 25/45/70%).
- [ ] **Step 4: Live verification.** Deploy; Mobile mode; launch an NES title (the Task-5-era test ROM); `uitest.py touch tap` on the pad's Right/A zones → character moves/jumps (screenshot pair proves input landed); hold-slide on the dpad; menu toggle hides it; Desktop mode + auto → hidden. Screenshots `d1t6-*`. Restore any ini changes byte-identical.
- [ ] **Step 5: Commit** `feat: virtual gamepad overlay for in-process cores (D1 Task 6)`.

---

### Task 7: Close-out — walks, gates, spec status, final review, merge gate

**Files:**
- Modify: `docs/superpowers/specs/2026-07-21-formfactor-adaptivity-design.md` (Status), `.superpowers/sdd/progress.md` (ledger)

- [ ] **Step 1: TV-mode walk.** `display/mode=tv` (snapshot ini): full surface walk (home all-six-themes spot XMB/Grid/Lumen, browse, detail, settings hub + 3 panels, OSK, Esc menu, a reader, video player, profile switcher) — screenshots `d1t7-tv-*`; verify insets/scale everywhere, focus visible, NOTHING clipped by the inset.
- [ ] **Step 2: Mobile-mode walk** via `uitest.py touch` end-to-end: tap-navigate home → detail → play video → tap chrome → back via edge-swipe → settings flick-scroll → OSK text entry (touch-sized keys) → reader gestures → NES + virtual pad. Screenshots `d1t7-mob-*`.
- [ ] **Step 3: Desktop identity regression.** `display/mode` absent (default auto): pixel-compare screenshots of home/panel/OSK vs pre-branch baselines (build main, shoot the same states, byte-or-near-identical); full probe suite; perfbaseline 3 runs (startup medians ±20%, nav.select flat); watchdog zero unexplained; the QML no-direct-selection-writes grep green.
- [ ] **Step 4: Spec status** → `Phase 1 complete: TV + Mobile modes on desktop; auto+override; touch on the contract. Phase 2 (Android provisioning) next.` + follow-ups list (theme-element flick if deferred, ebook pinch if skipped, swipe-volume). Commit `formfactor: close out Phase 1`.
- [ ] **Step 5: Final whole-branch review** (fable, main..formfactor/d1-plan): cross-task seams — token identity discipline (Desktop = pixel no-op), touch-vs-graph authority (no bypasses), gesture/timer lifecycle (double-tap state, flick vs tap disambiguation, virtual pad mask stuck-bit on focus loss), uitest touch determinism. Fix rounds until Merge-ready.
- [ ] **Step 6: Merge gate.** Present to the user: merge+push+deploy per the standing pattern.

## Self-Review (done at write time)

- Spec coverage: §1 FormFactor→T1/T3; §2 TV pass→T2/T3; §3 touch model→T4; §4 player/reader→T5; §5 pad→T6; §6 verification→every task + T7. Deferred-by-spec items (swipe-volume, per-core pads) stay out.
- Ambiguities resolved here: mobile tap = one-tap activate (desktop keeps two-step); tap zones/gestures are touch-only (mouse behavior frozen); pinch scoped to comic/PDF; theme-element flick is investigate-then-maybe-defer (no custom physics).
- Type consistency: `FormFactor::instance()`, token names, `hitClamp`, `setVirtualPadMask`, uitest `touch tap|flick|pinch` used identically across tasks.
