# Controller-aware UI

When a controller is in use the app should look like a console: no mouse pointer, and every on-screen
hotkey hint naming the controller button to press rather than a keyboard key. When a mouse is in use it
should look like a desktop app again. Nothing about this is configurable — the app follows the device the
user actually touched.

## Today

Three facts about the current tree shape this design.

`MainWindow::pollMenuPad` is the single place a controller reaches the browse UI. It polls `Gamepad`
(SDL) on a timer, turns seven RetroPad buttons into Qt nav keys, and delivers them through `sendNavKey`,
which already marks the delivery controller-origin with `NavContext::SyntheticScope`. Every controller
press in the menus passes through this one function.

On-screen hotkey hints live almost entirely in the themed `helpsystem` element
(`native/src/theme2/qml/elements/HelpSystem.qml`): a centred row of chips, each a `button` string and a
`label`. The `button` strings are hardcoded keyboard text authored in each theme's JSON — `"Enter"`,
`"Esc"`, `"I"`, `"/"`, `"T"`, `"←↑↓→"` — and themes come from a public registry, so the app does not
control what a chip says.

Cursor hiding exists only inside the video player's idle timer. There is no app-wide notion of the
pointer being irrelevant.

`FormFactor` is the precedent for a singleton `QObject` exposed to QML as a context property (`form`) and
consumed by widgets on the C++ side. This design follows it exactly.

## The input authority

`native/src/input/InputMode.{h,cpp}` — a singleton `QObject`, the one authority on which device the user
is driving.

```
Q_PROPERTY(QString mode  READ modeName NOTIFY changed)   // "pointer" | "pad"
Q_PROPERTY(QString brand READ brand    NOTIFY changed)   // "xbox" | "playstation" | "switch" | "generic"
Q_INVOKABLE QString chipFor(const QString& hintKey) const;
void notePad();       // a controller press happened
void notePointer();   // a real mouse movement happened
```

It is registered as `input` next to `form` in all four hosts that install context properties —
`ThemeEngine`, `ThemedPanelHost`, `ThemePickerHost`, `ReaderChromeHost` — so every themed surface and
every panel sees the same object.

**Entering pad mode.** `pollMenuPad` calls `notePad()` on any button press edge it observes. It already
sees them all, so no second polling path is introduced.

**Leaving pad mode.** A window-level event filter calls `notePointer()` on a **spontaneous**
`QMouseEvent` whose global position differs from the last one seen. Both conditions matter: synthetic
moves (uitest injection, Qt's own enter/leave from a layout change under a stationary cursor) must not
flip the mode back, or a themed animation would un-hide the cursor mid-navigation. A physical keypress
changes nothing — a keyboard on a couch is not a mouse.

**Startup** is pointer mode, even with a controller already connected. The mode follows use, not
presence.

**Brand** comes from `SDL_GameControllerGetType` on the port that last sent input, via a new
`Gamepad::brand(unsigned port)` accessor. `describeControllers()` already reads the type for its
diagnostic line, so the SDL call is established. An unrecognised pad resolves to `generic`, which uses
the Xbox labels — the de-facto lingua franca in frontends.

## Hint translation

`native/src/input/PadGlyphs.{h,cpp}` holds the translation as pure functions: no SDL, no Qt widgets, no
window. This is what a probe tests.

The chain is

```
hint string  ->  UI verb  ->  RetroPad id  ->  live binding code  ->  brand label
```

| hint | verb     | RetroPad   | default SDL | xbox | playstation | switch |
|------|----------|------------|-------------|------|-------------|--------|
| `Enter` | Confirm | B (south) | 0 | A | ✕ | B |
| `Esc`   | Back    | A (east)  | 1 | B | ○ | A |
| `I`     | Details | X (north) | 3 | Y | △ | X |
| `/`     | Search  | Y (west)  | 2 | X | □ | Y |
| `F`     | Filter  | L         | 9 | LB | L1 | L |
| `P`     | Playlist| R         | 10 | RB | R1 | R |
| `T`     | Theme   | Select    | 4 | ⧉ | Create | − |
| `S`     | Skip    | Y (west)  | 2 | X | □ | Y |
| arrows  | D-pad   | —         | — | (unchanged) | (unchanged) | (unchanged) |

`S` is the player surface's verb only; every other row is the browse surface. Search and Skip share the
west button because they never appear on the same screen — see "New pad bindings" below.

The **live** binding is what is rendered, not the factory one: the SDL code comes from
`Gamepad::binding(port, retroId)`, so a user who remapped a button in the input panel sees the button
they actually mapped. Arrow chips (`←`, `↑↓`, `←→`, `↑↓←→`, `↑↓→`) pass through unchanged — a D-pad
arrow is already the right glyph.

A `button` string that is not in the table falls through **unchanged**. A third-party theme's own chip is
left alone rather than guessed at or hidden; showing the author's text is the honest failure mode.

`HelpSystem.qml` changes in one place:

```qml
text: (typeof input !== "undefined" && input && input.mode === "pad")
        ? input.chipFor(modelData.button) : (modelData.button ? modelData.button : "")
```

The `typeof` guard matches every existing `form` consumer, so a fixture loaded without `input` renders
exactly as it does today. The `label` half of each chip never changes — the verb is the same verb.

## New pad bindings

The verbs in the table above that have no controller button today get one. `pollMenuPad`'s nav table
gains five rows for the browse surface:

| RetroPad | key sent | verb |
|----------|----------|------|
| X (north) | `Key_I`     | Details |
| Y (west)  | `Key_Slash` | Search |
| L         | `Key_F`     | Filter |
| R         | `Key_P`     | Add to playlist |
| Select    | `Key_T`     | Cycle theme |

All non-repeating. All read through `Gamepad::binding()`, so they are remappable per port through the
input panel that already exists. Nothing existing changes meaning: south, east, Start and the D-pad keep
their current jobs. The keys route through `sendNavKey`, which delivers to whatever surface is topmost,
so they arrive at `ThemeView`'s key handler indistinguishable from a keyboard press — no new routing.

`pollMenuPad` selects its table by surface. The video player gets its own two rows, North → `Key_I` (mark
a segment) and West → `Key_S` (skip the offered segment), which are the player's two real keyboard verbs.
The shape is the same on both surfaces — north is the info/mark button, west is the secondary action — so
the two tables do not compete for muscle memory.

## The cursor

Entering pad mode calls `QApplication::setOverrideCursor(Qt::BlankCursor)`; leaving calls
`QApplication::restoreOverrideCursor()`.

An override cursor outranks per-widget cursors, so the twenty-odd `setCursor(Qt::PointingHandCursor)`
calls across `HomeView` and `MainWindow` need no changes, and the video player's own idle hide/show keeps
running underneath without effect — its `unsetCursor()` on control-show reveals nothing while the
override is up, which is the wanted behaviour.

Hiding the pointer does not disable it: a blind click still lands. The first real mouse movement restores
the cursor and the keyboard chips together, in one `changed()` emission.

## Player and OSK text

Three player strings name a keyboard key in prose and are visible on screen (not tooltips):

- `MainWindow.cpp:17151` and `MainWindow.cpp:18743` — "While a video is playing: S skips the offered
  segment, I marks where one starts and ends."
- `MainWindow.cpp:22251` — "Intro starts here. Press I again at the end."

Each takes `%1`/`%2` placeholders filled from `chipFor()`, so on a pad the first reads "Ⓧ skips the
offered segment, Ⓨ marks where one starts and ends". They rebuild on `InputMode::changed()`.

The player's hover tooltips (`Screenshot (F12)`, `Toggle full screen (F11)`, `[ and ]`) are left alone. A
tooltip only appears under a pointer, so it is never seen in pad mode.

The on-screen keyboard's footer (`Osk.cpp:180`) is currently controller-worded unconditionally —
`"B: delete   Start: done"`. It becomes mode-aware in both directions: keyboard wording in pointer mode,
brand-correct glyphs in pad mode. It reads the same singleton from C++ and reconnects
on `changed()`.

## Out of scope

The classic (non-themed) QWidget views keep their baked-in keyboard hints — `CarouselView`'s
"← → spin · Enter to open" and `LibraryView`'s "Enter to play." Those surfaces are the desktop path and
are not part of this change.

No user setting is added. Detection is device-driven and unambiguous, so there is nothing to configure,
and no row has to be added to the two settings builders.

## Testing

`probe_padglyph` — a pure probe over `PadGlyphs`, no window and no SDL. It asserts:

- every hint string in the table resolves to its per-brand label, for all four brands;
- a remapped binding changes the rendered glyph (bind Details to the east button, expect the east label);
- an unknown `button` string returns itself unchanged;
- arrow chips pass through untouched.

Registered in all three required places: the `add_executable`/`target_link_libraries` pair in
`native/CMakeLists.txt`, the no-argument runner loop in `native/tools/run-headless-probes.sh`, and the
`--target` list in the CI "Build probes" step. Missing any one leaves the probe silently never running.

A source-level gate in the runner asserts that every `button` string in the **bundled** themes
(`native/themes2/*/theme.json` and the fallback theme in `Theme.js`) resolves to a verb, so a new bundled
theme cannot ship a chip the translator does not know. Registry themes stay on the pass-through
fallback and are not gated.

Live verification under `EB_UITEST=1`, driven through `native/tools/uitest.py`: on the themed home, in
the video player and with the OSK open — a pad press hides the cursor and flips the chips to glyphs; a
mouse move brings both back.
