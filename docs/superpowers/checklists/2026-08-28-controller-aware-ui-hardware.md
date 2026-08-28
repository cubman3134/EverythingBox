# Controller-aware UI — hardware verification checklist

The controller-aware UI ([spec](../specs/2026-08-28-controller-aware-ui-design.md),
[plan](../plans/2026-08-28-controller-aware-ui.md)) is gated by five probes and a source-level gate, but the
part a user actually experiences — a real controller press, the cursor leaving the screen, a pad hot-swapped
mid-session — is invisible to all of them. `pollMenuPad` reads SDL directly, and the `EB_UITEST` channel
injects Qt events, which never reach `Gamepad`; the channel's `inputmode` command drives `InputMode` directly
so the chips and the cursor can be photographed, but nothing in software can press a button.

This is the list a person walks with a controller on the desk. It is written down here, tracked, rather than
left in a scratch report, because it is the only verification this feature has for two thirds of its surface.

actually ran and looked at in this pass; the qualifier after it says what the harness still could not do.

### A. Mode and cursor

1. **Cursor hides on a pad press** — everywhere in the app, including over the themed QQuickWidget.
   *VERIFIED-BY-HARNESS* (live cursor-handle A/B, §3.4) — **but the trigger was the test command, not a
   physical button.**
2. **Cursor comes back on a real mouse move, and only on a real one** — must not reappear during a `key`/
   `state` sequence, nor when a themed slide animation moves widgets under a stationary pointer.
   *VERIFIED-BY-HARNESS* for the OS-level move and for the "not during key injection" half; **NEEDS-HARDWARE**
   for a physical mouse and for the slide-animation case.
3. **No cursor flicker / no stuck cursor** on alternating pad presses (unbalanced-override symptom).
   *VERIFIED-BY-HARNESS* (6× pad then 1× pointer leaves the arrow) — **NEEDS-HARDWARE** for the visual
   "does it blink" judgement, and for the §3.4 lag caveat.
4. **The stationary-move seed** — boot with the pointer parked *outside* the window, press a pad button,
   navigate through a themed slide animation: the cursor must stay hidden. **NEEDS-HARDWARE** (cold boot).
5. **The seeded pointer watch, pointer parked *inside* the window** — same, and this is the case the old
   sentinel got wrong. **NEEDS-HARDWARE.**
6. **Wheel leaves pad mode** — with a pad driving and the cursor hidden, turn the mouse wheel over a list:
   cursor returns, chips re-spell. **NEEDS-HARDWARE.**
7. **Teardown in pad mode** — drive with a controller until the cursor is hidden, then quit. The cursor must
   be normal in whatever is behind the app, immediately. **NEEDS-HARDWARE.**
8. **The player's own idle cursor hide/show still works** once back in pointer mode. **NEEDS-HARDWARE.**

### B. The help bar and the glyphs

9. **The bar reads keyboard keys with no pad ever pressed.** *VERIFIED-BY-HARNESS* (`01`, `18`, `14`).
10. **The bar re-spells to the brand's buttons in pad mode, and back.** *VERIFIED-BY-HARNESS* on the themed
    home, the Night bar, and the built-in fallback bar, in generic/PlayStation/Switch — **NEEDS-HARDWARE**
    for the transition being driven by a real button.
11. **The five new glyphs are not tofu.** *VERIFIED-BY-HARNESS* — all five render (§3.3). The only open
    question is the small `□`, which is a design call, not a bug.
12. **Mixed-brand couch** — Xbox on port 0, PlayStation on port 1. Pressing on port 1 must re-spell to
    PlayStation and back on port 0, with no stutter (a 60 Hz re-bind would collapse the frame rate).
    **NEEDS-HARDWARE.** This is the specific failure the edge-only rule exists to prevent and the only place
    it can be observed.
13. **Hot-swap** — unplug the driving pad, plug in a different-brand one, press a button: the chips must
    re-spell, proving the once-only `setPad` did not pin the brand for the session. **NEEDS-HARDWARE.**
14. **A remap re-spells the chips** — rebind a button in the input panel; the bar must follow with no
    UI-side `notifyBindingsChanged()` call existing. **NEEDS-HARDWARE** (needs a live `Gamepad` with a
    written map).

### C. The buttons themselves

15. **The five new browse buttons on the THEMED home**: North → Details (`I`), West → Search (`/`),
    L → Filter (`F`), R → Add to playlist (`P`), Select → Cycle theme (`T`). **NEEDS-HARDWARE.**
16. **The two new player buttons**: North → segment marks (`I`), West → skip the offered segment (`S`); and
    L / R / Select do nothing there. **NEEDS-HARDWARE.**
17. **Nothing regressed on the original seven**: D-pad repeat still 420/160 ms, B = Enter, A = Back, Start
    still opens the browse context menu (and Escape/close over an overlay). **NEEDS-HARDWARE.**
18. **Held across a surface change** — hold L on the player, navigate back to browse still holding: the
    Filter panel must NOT open; release and press again, it must. **NEEDS-HARDWARE.**
19. **R on the themed GRID browse view** — drill into a catalog on a grid theme and press R: the playlist
    picker must open for the highlighted row. **NEEDS-HARDWARE.**
20. **R on an empty themed grid-browse level** (only a guidance row on screen): nothing must happen — no
    picker, no toast, no playlist entry; then check the playlist is still clean. **NEEDS-HARDWARE.**
21. **R from the themed sidebar** — in the categories rail, R must do nothing; cross back to the grid and R
    must open the picker. **NEEDS-HARDWARE.**
22. **Classic (non-themed) home**: `I` / `/` / `F` / `T` are *silently* inert (no beep, no stray
    navigation); `P` is **not** inert — R opens the playlist picker, which must be d-pad drivable and
    dismissable **with the controller alone**. **NEEDS-HARDWARE.**
23. **The readers (ebook / PDF / comic) and any open settings panel** must be unharmed by the new buttons.
    **NEEDS-HARDWARE.**

### D. The prose

24. **Both settings surfaces render the `%1`/`%2` string without layout damage**, themed and classic.
    *VERIFIED-BY-HARNESS* (`settings-hint-stack.png`, `classic-hint-stack.png` — no wrap damage, no clipping,
    the glyph is narrower than the letter it replaces so the line only gets shorter).
25. **The pad wording names the right letters** — on an Xbox pad, "…**X** skips…, **Y** marks…".
    *VERIFIED-BY-HARNESS*: the app renders `X` (Skip, retro id 1 → SDL 2) and `Y` (Details, retro id 9 →
    SDL 3). **Note for the record: Task 5's report predicted "Y skips…, X marks…" — that prediction was
    backwards; the running app is right and the note was wrong.**
26. **The OSK footer in both modes** — `Backspace: delete   Enter: done` with no parenthetical on a mouse;
    `B: delete   Menu: done   (a real keyboard types directly)` on an Xbox pad. *VERIFIED-BY-HARNESS*
    (`osk-footer-stack.png`), including the PlayStation (`○ / Options`) and Switch (`B / +`) spellings —
    so Task 5's carried-forward item 1 is answered: it reads `Menu`/`Options`/`+`, never the literal `Start`.
27. **The OSK footer re-words LIVE** — with the keyboard open, the footer must flip without the keyboard
    closing. *VERIFIED-BY-HARNESS* (the four OSK shots are one continuous session with the OSK never closed).
    **NEEDS-HARDWARE** for the flip being caused by a real button press.
28. **The OSK footer's teardown** — open and close the keyboard several times while changing mode.
    *VERIFIED-BY-HARNESS* (3 cycles, no crash, channel still `ok ready`).
29. **The remap case behind the OSK fix** — rebind RetroPad START and confirm the footer's done arm follows.
    **NEEDS-HARDWARE.**
30. **The player's notice** ("Press %1 again at the end") — needs a real video with a marked intro to appear
    at all. **NEEDS-HARDWARE.**
31. **The passcode pad's footer in both modes**, and that it re-words live mid-entry; and that its
    `⌫` / `✕` / `●` glyphs still render after the `fromUtf8` conversion. **NEEDS-HARDWARE** (no passcode is
    configured on this machine, so the pad never appears).
32. **`lupdate` extraction** — the new `%1`/`%2` msgids are unverified against any `.ts` file and the old
    msgids are now orphaned. **NEEDS-HARDWARE** only in the sense that nobody has run it; it is a build task,
    not a controller one.

### E. Known gaps — EXPECTED BEHAVIOUR, not findings

Both of these are written into the source as KNOWN gaps. They are here so a tester who trips over one
records it as expected rather than filing it.

33. **The cursor stays visible for the rest of the session if you nudge the mouse mid-game.** Start a game,
    move the physical mouse: the arrow appears over the emulator and does *not* go away again when you put
    the mouse down — it only re-hides on the next pad press back in the menus. **EXPECTED.** The cursor
    swings only on `notePad()`/`notePointer()`, and `notePad` cannot fire in-game: `pollMenuPad` returns
    early there (the emulator owns the pad) and nothing in `src/emu` or `src/retropark` touches the cursor.
    Documented at the `KNOWN GAP, not a regression` comment on the `InputMode::changed` connection in
    `MainWindow.cpp` (~1035-1041). Before this arc there was no cursor hiding anywhere, so nothing got worse.
34. **The screensaver can still come up while you are pressing L / R / Select on the player.** Sit on the
    video player and press only those three for longer than the attract timeout: attract mode appears under
    a moving thumb. **EXPECTED.** `noteAttractInput()` lives inside `sendNavKey`, and a nav row whose key is
    `0` on the current surface never reaches it — L / R / Select are inert on the player. The `notePad()`
    edge above it *did* fire, so the cursor and the help chips are correct for those presses; it is only the
    idle clock that misses them. Documented at the `KNOWN, and deliberate for now` comment inside the nav
    loop in `MainWindow.cpp` (~3913-3918 pre-fix; the comment moved down with this pass's static_asserts).

---

