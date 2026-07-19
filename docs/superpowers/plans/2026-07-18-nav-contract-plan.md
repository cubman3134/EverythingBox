# Nav Contract Implementation Plan (Phase 3, Subsystem A)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A C++-owned NavGraph gives the themed surface enforced invariants — selection never lost, full reachability, one Back router terminating at the exit menu, two-state inputs — CI-gated by probe_navqml, plus a black-frame watchdog with recovery.

**Architecture:** NavGraph (C++, `src/ui/nav/`) owns selection state and the back stack; the themed QML keeps its existing prop names (`currentIndex`/`catIndex`/`focusZone`/…) as the RENDER interface — NavGraph writes them, QML-side mutations become requests. One keypress site (`ThemeView.qml` root `Keys.onPressed`) delegates to NavGraph; MainWindow's `goBack` themed branch and the QML Esc branch both fold into `NavGraph::back()`. Spec: `docs/superpowers/specs/2026-07-18-nav-contract-design.md`.

**Tech Stack:** Qt 6 C++ (QObject exposed to QML via `setProperty`/`setContextProperty`), QML, headless probe pattern.

**Spec adaptation (documented deviation, mirror of plan-2's builder-pattern precedent):** the spec sketches per-element `NavItem` attached registration. The CURRENT themed surface is Repeater-driven (rows/tiles are data, not persistent items), so this plan registers **zones** (category rail, item column, button bar, action chooser — each with a count property and geometry role); selection = (zone, index), and the invariants hold at that granularity. The per-element `NavItem` attached API ships with subsystem B when surfaces are rebuilt as persistent elements; the plan's close-out records this in the spec.

## Global Constraints

- Invariants (spec, binding): (1) `selectedId`/(zone,index) always names a live registered element — null unrepresentable in the public API; (2) movement graph reaches every registered zone/element from the default; (3) movement resolves spatially in ONE shared resolver; (4) repeated Back reaches the root exit menu in bounded steps.
- Behavior parity except: focus-loss bugs become impossible; Back at XMB root reliably opens the exit menu. The themed surface must LOOK and animate identically (the glide/scale animations bind to `currentIndex`/`catIndex`/`catScroll`/`itemScroll` — keep feeding the same props).
- Responsiveness: `nav.select` median must stay within 2 ms of the current baseline (`perfbaseline.py`, medians, 3 runs).
- probe_nav (classic kit) stays green untouched; probe_navqml joins `run-headless-probes.sh` AND `.github/workflows/ci.yml`'s build-target list.
- Machine facts (as all prior tracks): build `cmake --build build --config Release [--target <t>]`; probes need Qt bin (+ `/c/mpv-dev` for the runner) on PATH + `QT_QPA_PLATFORM=offscreen`; deploy to `C:\MyMediaVault-app` keeping the exe name; `MMV_UITEST=1` + `python native/tools/uitest.py` only, never SendKeys/focus; protect user data; `rtk` proxy normal.
- The themed code is `#ifdef MMV_HAVE_QML` — NavGraph core is NOT (it's plain QObject, testable without QML); only the ThemeView wiring is guarded.
- Scout map (authoritative anchors, verified 2026-07-18): key delivery `MainWindow::sendNavKey` :875-928 (themed branch :922, `deliver` sends QKeyEvent to the QQuickWidget); ALL themed key handling is `ThemeView.qml:189-252` root `Keys.onPressed` (Xmb.qml is presentational, no Keys); selection props on the ThemeView root: `currentIndex` :18, `catIndex`/`categories` :45-46, `actionsOpen/actionIndex/actionItem/actionFav` :52-55, `focusZone`/`buttonIndex` :68-69; divider auto-hop `onCurrentIndexChanged` :138-147 (`seekSelectable`); mouse `gotoItem/gotoCat/buttonAction` :113-132 (each calls `forceActiveFocus()`); `WheelHandler` :179-188; `themedOnBack_` writes MainWindow.cpp:1980/2223/2324, read :973; QML's OWN Esc branch ThemeView.qml:241-245; Osk = widget NavOverlay subclass, `Osk::getText` nested QEventLoop Osk.cpp:166-175; focus revival today = dual path (NavOverlay.cpp:87-93 `mmvQuickRoot` kick + MainWindow.cpp:850-861 esc-menu handler); NavContext registers a NULL ring for themed pages (Nav.h:136); frame grab trick = `MainWindow::grab()` (works occluded, software backend — MainWindow.cpp:1036-1113); legitimate-black flag `inContent_` MainWindow.cpp:362-364; ThemeBridge signal fan-out ThemeEngine.cpp:187-198; props seeded ThemeEngine.cpp:151-156; `mmvQuickRoot` marker :206.

---

### Task 1: NavGraph core + probe_navqml (invariants 1/2/4, headless, no QML)

**Files:**
- Create: `native/src/ui/nav/NavGraph.h`, `native/src/ui/nav/NavGraph.cpp`
- Create: `native/tools/probe_navqml.cpp`
- Modify: `native/CMakeLists.txt` (app source list + probe target), `native/tools/run-headless-probes.sh` (add `"probe_navqml NAVQML-OK"` to the new-probes loop), `.github/workflows/ci.yml:52` (append `probe_navqml`)

**Interfaces (later tasks rely on these exact signatures):**

```cpp
// One selection model + back stack for a themed screen. Selection is (zone, index) and can
// never be null once a zone is registered: element churn reassigns deterministically.
class NavGraph : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString zone READ zone NOTIFY selectionChanged)
    Q_PROPERTY(int index READ index NOTIFY selectionChanged)
public:
    explicit NavGraph(QObject* parent = nullptr);

    // Zone registry. `count` may change any time via setZoneCount (Repeater data swaps).
    // `row`/`col` place the zone on a coarse grid for spatial arrow resolution (Invariant 3).
    // axis: which arrows step the index WITHIN the zone (Horizontal = Left/Right, Vertical =
    // Up/Down); the other axis crosses zones. XMB: categories = Horizontal, items = Vertical.
    void registerZone(const QString& id, int count, int row, int col,
                      Qt::Orientation axis = Qt::Horizontal, bool wraps = false);
    void setZoneCount(const QString& id, int count);   // 0 hides the zone (selection reassigns away)
    void removeZone(const QString& id);
    void setDefaultZone(const QString& id);

    // Non-selectable indices (dividers): the resolver skips them; a set index snaps to the
    // nearest selectable — the model owns what QML's seekSelectable did.
    void setUnselectable(const QString& zone, const QSet<int>& indices);

    QString zone() const;   int index() const;
    bool move(Qt::Key arrow);                 // spatial step; returns false if nothing changed
    void select(const QString& zone, int index); // request (clamped + divider-snapped)
    void activate();                          // emits activated(zone, index)

    // Back stack (Invariant 4). Root behavior: back() at empty stack emits rootBack()
    // (host opens the exit menu). Levels: screens, drills, overlays (esc menu, NavMenu, OSK).
    void pushLevel(const QString& name, std::function<void()> onPop);
    void popLevel();                          // runs onPop
    int  levelDepth() const;
    bool back();                              // pops one level, or emits rootBack(); always true

    bool validate(QString* whyNot = nullptr) const; // Invariant 2: zone graph connected, counts sane

signals:
    void selectionChanged(const QString& zone, int index);
    void activated(const QString& zone, int index);
    void rootBack();
    void levelsChanged(int depth);
};
```

Semantics (binding): after the first `registerZone`, `zone()`/`index()` are ALWAYS valid — `setZoneCount(id, 0)` or `removeZone` on the selected zone reassigns to the nearest zone by grid distance (then default zone); index clamps into the new count and snaps off unselectable entries; there is no null state and no API to produce one.

- [ ] **Step 1: Write the failing probe** — `native/tools/probe_navqml.cpp`, pattern of probe_nav (offscreen QApplication, CHECK macro, sentinel `NAVQML-OK`). Assert, minimum: selection valid after first registerZone; churn storm (grow/shrink/zero/remove zones ×1000, randomized with a fixed seed) never yields invalid selection (validate after every mutation); divider snap (unselectable set) never selects a divider and always terminates; move() reaches every zone from default (walk all arrows breadth-first, compare against registry — Invariant 2); back(): push 5 levels, 5 backs pop in order, 6th emits rootBack; onPop ordering LIFO; bounded (no re-push loops: pushing inside onPop is IGNORED during a pop — assert). Build target in CMake: sources `tools/probe_navqml.cpp src/ui/nav/NavGraph.cpp src/ui/nav/NavGraph.h`, link Qt6::Core Qt6::Gui.
- [ ] **Step 2: RED** — `cmake --build build --config Release --target probe_navqml` fails on missing NavGraph.h.
- [ ] **Step 3: Implement NavGraph** per the header above. Keep it under ~350 lines; spatial resolution = grid rows/cols with nearest-by-distance fallback; document the reassignment order in the header comment.
- [ ] **Step 4: GREEN** — probe prints NAVQML-OK. Register in runner + ci.yml. App target still builds (NavGraph.cpp/h in the source list).
- [ ] **Step 5: Commit** — `nav: NavGraph — the themed selection model + back stack, invariants probe-gated`

---

### Task 2: ThemeView on the contract (key routing, selection arbitration, NavContext registration)

**Files:**
- Modify: `native/src/theme2/ThemeEngine.cpp` (`buildView`: create + expose the NavGraph), `native/src/theme2/ThemeEngine.h`
- Modify: `native/src/theme2/qml/ThemeView.qml` (Keys.onPressed delegates; mouse/wheel become requests; divider-skip removed in favor of the model's)
- Modify: `native/src/ui/MainWindow.cpp` (themed pages register a REAL NavContext presence; selection reads/writes go through the graph)

**Interfaces:**
- Consumes: Task 1's NavGraph exactly. Produces: the QML-visible object `nav` (root context property via `view->rootContext()->setContextProperty("nav", graph)` — ThemeEngine owns the graph, one per built view) with `nav.move(key)`, `nav.select(zone, index)`, `nav.activate()`, `nav.back()`, `nav.zone`, `nav.index`. Zone ids (exact strings Tasks 3-4 use): `"categories"`, `"items"`, `"buttons"`, `"actions"`.
- The RENDER interface stays: NavGraph's `selectionChanged` handler in ThemeEngine writes the SAME QML props QML binds today (`currentIndex` when zone=="items", `catIndex` when zone=="categories", `buttonIndex`/`focusZone`, `actionIndex`) so every animation (`catScroll`/`itemScroll` glides, row scale/opacity, `col.kick()`) keeps working untouched.

- [ ] **Step 1: ThemeEngine wiring.** In `buildView` (ThemeEngine.cpp:125-208): create `auto* graph = new NavGraph(container)`; `setContextProperty("nav", graph)` BEFORE `setSource` (context properties must precede load); register the zones with counts from the seeded props (`items`/`categories`/`buttonList` sizes — re-sync via `setZoneCount` wherever the host updates those props today: the `setProperty("items", …)` sites in MainWindow.cpp:2013/2142/2157/2188/2281 gain a paired `graph->setZoneCount("items", n)`; expose the graph to the host: `NavGraph* ThemeEngine::navGraph(QWidget* view)` accessor mirroring `rootItem()`). Wire `selectionChanged` → the prop-write bridge described above (in ThemeEngine, one function, ~20 lines). Wire `activated` → the existing ThemeBridge `activated` fan-out path.
- [ ] **Step 2: QML delegation.** `ThemeView.qml:189-252` Keys.onPressed: arrows/Enter → `nav.move(...)` / `nav.activate()`; the local `step()/stepCat()/vstep()` mutations and `focusZone` juggling are DELETED for key input (functions may remain for a release or two if mouse paths still call them — see next). Esc branch (:241-245) → `nav.back()`. Mouse `gotoItem/gotoCat/buttonAction` (:113-132) → `nav.select("items", index)` + `nav.activate()` etc. — QML never writes the selection props directly again (grep the file: zero assignments to `currentIndex`/`catIndex`/`buttonIndex`/`actionIndex` outside the nav-driven bridge writes). WheelHandler (:179-188) → `nav.move(...)`. The divider-skip `onCurrentIndexChanged` block (:138-147) is DELETED — the host now feeds `setUnselectable("items", dividerIndexSet)` wherever it builds the items list (find the divider markers in the items payload — the `_open`-style/divider rows; MainWindow's fillXmbFromItems/browse sync sites). The `onItemsChanged` reset side-effects (:137) move behind the graph: `setZoneCount` triggers reassignment; `actionsOpen` reset stays QML-local (render state, not selection).
- [ ] **Step 3: NavContext registration.** MainWindow: where themed pages currently get the null ring (`updateNavForPage`, cf. Nav.h:136 comment), register the graph instead so the kit knows the themed page HAS selection (a thin adapter or a `NavContext::setActiveGraph(NavGraph*)` — add the minimal hook to Nav.h; do NOT restructure NavContext). `sendNavKey`'s themed branch (:922) keeps delivering the QKeyEvent (QML routes it to nav.*) — no change needed there beyond verifying the chain.
- [ ] **Step 4: Parity verification (this is the gate).** Build, deploy, run the FULL standard route live: category left/right, item up/down incl. divider hop-over, enter/activate on game (NavMenu opens), buttons zone reachable, action chooser arrows + choose, wheel scroll, mouse click selection, search opens OSK, metadata panel follows selection, animations identical (capture the same trans pairs as the polish sweep and compare motion presence). probe_nav + probe_navqml + full suite green. `perfbaseline.py` 3 runs: nav.select median within 2 ms of the pre-task value (record both in the report).
- [ ] **Step 5: Commit** — `nav: ThemeView runs on the NavGraph contract — one resolver, selection arbitrated in C++`

---

### Task 3: One Back router

**Files:**
- Modify: `native/src/ui/MainWindow.cpp` (goBack :963-993 themed branch; the three `themedOnBack_` closure writes :1980/:2223/:2324 become `pushLevel` registrations; `themedOnBack_` member DELETED from MainWindow.h)
- Modify: `native/src/ui/nav/NavOverlay.cpp` (overlay open/close notifies the graph as a level — push on show, pop on dismiss; the dual focus-revival paths :87-93 collapse into the graph's `levelsChanged` handler in ThemeEngine doing the ONE `mmvQuickRoot` revival)
- Modify: `native/src/ui/MainWindow.cpp` esc-menu handler :850-861 (drop the redundant kick — the graph revival owns it)

**Interfaces:** Consumes NavGraph's back stack + `rootBack`. Produces: the app-wide guarantee — every themed level (XMB drill, browse level, catalog re-select, detail return) is a `pushLevel` whose onPop performs exactly what the old closure branch did (`home_->browseBack()`, `showCatalogs(cat, idx)` re-select, `showThemedHome()`, detail-return); `rootBack` → `showEscMenu()`. The Esc menu itself and the OSK push levels too (Back inside them closes them first — Invariant "Back always closes the topmost thing").

- [ ] **Step 1:** Map every current themed back behavior (the scout's §2 table) to pushLevel calls at the sites that ENTER those states (drill-in push, browse-level push per level, detail push). goBack's themed branch becomes `if (themed) { navGraph->back(); return; }`. Delete `themedOnBack_`.
- [ ] **Step 2:** Overlays as levels: NavOverlay base gains optional graph notification (constructor arg or setter — nullptr for classic surfaces keeps behavior identical); Osk and esc-menu and themed NavMenus pass the graph. Verify the OSK-close focus revival now happens exactly once, from the graph handler.
- [ ] **Step 3:** probe_navqml additions: scripted level pushes mimicking the real screens (XMB root → catalog → browse ×3 → detail), then Back×N: asserts each pop's onPop ran in order and the (N+1)th Back emits rootBack; overlay-push then Back closes overlay first.
- [ ] **Step 4:** Live: from deep in a browse drill, hammer Back to root — exit menu appears; Resume returns; Back inside OSK closes OSK only; Back inside NavMenu closes it only; second-search-focus and OSK-focus scenarios (the old bugs) exercised 5× each — selection always visibly present after close (uitest state shows the selected element each time). Suite green.
- [ ] **Step 5: Commit** — `nav: one Back router — themed levels on the NavGraph stack, exit menu at root, themedOnBack_ deleted`

---

### Task 4: Two-state themed inputs (components for B, contract-proven now)

**Files:**
- Create: `native/src/theme2/qml/elements/ThemedTextField.qml`, `native/src/theme2/qml/elements/ThemedChoice.qml`
- Modify: `native/src/theme2/theme2.qrc` (register the new files — REMEMBER the project gotcha: .qrc must be wired via the existing explicit compile, check how theme2.qrc is built in CMakeLists per the memory that target_sources-added .qrc silently no-ops)
- Modify: `native/tools/probe_navqml.cpp` (two-state transition asserts — instantiate the components in an offscreen QQuickWidget with a stub `nav`)

**Interfaces:**
- Produces (subsystem B consumes; exact API): `ThemedTextField { navZone: "field1"; text; placeholder; onCommitted(text) }` and `ThemedChoice { navZone; options: []; currentOption; onChosen(index) }`. States: `selected` (outline from theme accent — reuse the existing selection styling from Xmb.qml rows), `editing` (TextField active w/ OSK on TV — the component calls `nav.requestEdit(navZone)` which the host answers by running `Osk::getText` and writing back; ThemedChoice opens its option list inline). Escape in editing → selected (commit nothing); Enter → commit + selected. Each registers itself as a 1-count zone on `Component.onCompleted` via `nav.registerZone(navZone, 1, navRow, navCol)` — the QML-side registration path that B's surfaces will use wholesale.
- First real consumer is subsystem B; THIS plan proves the components in probe_navqml and a hidden test harness only — no user-visible surface changes.

- [ ] **Step 1:** Failing probe asserts: component registers on completion; arrow moves select it; activate enters editing; Escape returns to selected without commit; Enter commits once with the entered value; while editing, arrows do NOT move selection (keys go to the field).
- [ ] **Step 2:** Implement components (~80 lines each), qrc registration, GREEN.
- [ ] **Step 3: Commit** — `nav: two-state ThemedTextField/ThemedChoice — the input contract for themed surfaces`

---

### Task 5: Black-frame watchdog + hardening

**Files:**
- Create: `native/src/ui/BlackFrameWatchdog.h`, `native/src/ui/BlackFrameWatchdog.cpp`
- Modify: `native/src/ui/MainWindow.cpp` (instantiate when debug-gated; wire the skip contexts and the recovery kick)
- Modify: `native/tools/probe_navqml.cpp` (unit asserts for the pure classifier)

**Interfaces:**
- `class BlackFrameWatchdog : public QObject { BlackFrameWatchdog(std::function<QImage()> sampler, std::function<bool()> skip, QObject* parent); void start(); signals: void blackFrameDetected(int consecutive); }` — QTimer 1000 ms; `sampler` = downscaled `MainWindow::grab()` (grab→scaled(64,36) — cheap); `skip` = `inContent_ || launch-handoff || escMenu-transition` (host lambda); static pure classifier `static bool isBlack(const QImage&, double threshold = 0.99)` (luma < 16 on ≥99% of pixels) — probe-tested. On detection: log line to the existing debug log (`screen/state/timestamp`, reuse the mwLog idiom + uitest state summary), and on the SECOND consecutive detection fire the recovery kick (host: QQuickWidget scene refresh — `quickWidget->update()` + `mmvQuickRoot` polish; the exact context-loss kick to be determined from Qt docs at implementation, recorded in the report).
- Gating: created only when `UiTestServer::wanted()`-equivalent debug conditions hold (same gate: `MMV_UITEST` env / Settings ▸ Debug toggle) — zero cost otherwise.

- [ ] **Step 1:** Failing probe asserts on `isBlack`: all-black 64×36 → true; one bright row → false; dark-but-not-black (luma 40) → false; threshold edge.
- [ ] **Step 2:** Implement; wire in MainWindow (gated); GREEN; full suite.
- [ ] **Step 3:** Live: run the app 10 minutes on the standard route with the watchdog active — zero false positives (no detections during normal nav; deliberately cover a launch handoff and video playback — skipped contexts, no log lines). Kill.
- [ ] **Step 4: Commit** — `nav: black-frame watchdog — detect, log, and self-heal the blank-app state (debug-gated)`

---

### Task 6: Close-out — parity sweep, latency check, spec status

- [ ] **Step 1:** Full suite; `perfbaseline.py` 3 runs — nav.select median vs the recorded pre-Task-2 value (≤2 ms delta; include the table). polishsweep.py re-run: animations present, no regressions in the trans pairs; OSK/search/back flows all green.
- [ ] **Step 2:** Spec status → `Subsystem A complete: NavGraph invariants CI-gated (probe_navqml), one Back router, two-state components ready for B, watchdog live. NavItem per-element registration deferred to B (zone registration shipped — documented adaptation).` Also record the watchdog telemetry state (exit criterion: a week of telemetry OR one confirmed catch — note start date).
- [ ] **Step 3:** Commit — `nav: close out subsystem A — contract live, invariants gated, watchdog armed`
