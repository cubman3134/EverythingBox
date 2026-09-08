# Controllers in standalone emulators

EverythingBox runs some systems inside itself (libretro cores, RetroPark) and hands the rest to a standalone
emulator — Dolphin for GameCube and Wii, PCSX2 for PS2, DuckStation for PS1, Cemu for Wii U, ares, melonDS.
A standalone emulator is a separate program with its own controller settings, and out of the box most of them
launch with nothing bound: you get a game running and a pad that does nothing until you open that emulator's
input dialog and map every button by hand.

So EverythingBox writes the controller setup into each emulator's own config just before it boots. Four pads
plugged in become players 1–4, and the emulator's save-state and screenshot hotkeys land on the same keys the
in-app emulator uses.

This page is about the standalone tier. Controllers for the in-app tier are configured in the app itself.

## The switch: who owns this emulator's controller config

**Settings ▸ Stand Alone Emulators Settings**, under each emulator:

> **Let EverythingBox set up controllers**

On (the default) EverythingBox seats your pads and binds hotkeys in that emulator's own config each time it
launches a game. Off, it writes **nothing** for that emulator — not a player, not a hotkey, not a correction to
something it wrote earlier. Turning it off is the whole promise: if you have spent an evening building a
controller profile in Dolphin, no frontend should be allowed to improve it for you.

The switch is **per emulator**, because the person with a hand-built Dolphin profile usually still wants the
other five set up for them.

## The undo: `.eb-orig`

Before EverythingBox changes an emulator's input config for the first time, it copies the file next to itself
with `.eb-orig` on the end:

```
emulators/pcsx2/inis/PCSX2.ini            <- the live config
emulators/pcsx2/inis/PCSX2.ini.eb-orig    <- what it looked like before we ever touched it
```

That copy is written **once** and never refreshed, so it always holds the file as it was *before* EverythingBox
first changed it — not before the most recent change. To undo everything: close the emulator, delete the live
file, rename the `.eb-orig` copy back. You do not need EverythingBox to do it, which is the point.

Files EverythingBox created from nothing (a fresh install's config that did not exist yet) get no copy — the
way to undo creating a file is to delete it.

## Players 1–4

Pads are seated in the order the system reports them: the first is player 1, the second player 2, and so on up
to four. Each seat is written into the emulator's own config as that player, and only **when it is absent** —
a block you (or an earlier run) already wrote is never overwritten.

| Emulator    | Where the players are written                                   |
|-------------|-----------------------------------------------------------------|
| Dolphin     | `User/Config/GCPadNew.ini` `[GCPad1]`–`[GCPad4]`, plus `SIDevice0`–`SIDevice3` in `Dolphin.ini` |
| PCSX2       | `inis/PCSX2.ini` `[Pad1]`–`[Pad4]` on `SDL-0`–`SDL-3`            |
| DuckStation | `settings.ini` `[Pad1]`–`[Pad4]` on `SDL-0`–`SDL-3`              |
| Cemu        | `controllerProfiles/controller0.xml`–`controller3.xml`           |

PS1 and PS2 hardware only have two controller ports: players 3 and 4 are written faithfully but do nothing in
PCSX2 and DuckStation until you turn that emulator's **multitap** on. Dolphin and Cemu seat four natively.

## Hotkeys

The in-app emulator reserves three keys, and the standalone tier now gets the same three, so the muscle memory
transfers:

| | Key |
|---|---|
| Save state | **F2** |
| Load state | **F4** |
| Screenshot | **F12** |

(Start+Select on a pad closes a standalone emulator back to EverythingBox; that has always worked and is
handled by EverythingBox itself, not by the emulator's config.)

Which emulators get which, and why the rest get nothing:

| Emulator    | Save state | Load state | Screenshot | |
|-------------|:---:|:---:|:---:|---|
| PCSX2       | yes | yes | yes | `[Hotkeys]` in `inis/PCSX2.ini` |
| DuckStation | yes | yes | yes | `[Hotkeys]` in `settings.ini` |
| Dolphin     | — | — | — | see below |
| Cemu        | — | — | — | no configurable hotkeys, and no save states |
| everything else | — | — | — | not verified against its config format, so nothing is written |

Hotkeys are added **key by key, only if that key is not already bound**. An existing binding — the emulator's
own, or one you chose — is never replaced, and adding a binding can never remove one.

**Dolphin deliberately gets nothing.** Dolphin applies its built-in hotkey defaults only when `Hotkeys.ini` is
absent; the moment that file exists, every hotkey comes from it and anything the file does not name becomes
*unbound*. Writing three keys into it would silently cost you Esc-to-stop, fullscreen, frame advance and
Dolphin's own F1–F8 state slots. Dolphin already binds save, load and screenshot by default, so there is
nothing to gain and a whole hotkey set to lose.

Where an emulator does not support one of these, it gets nothing for it rather than an approximation.

## Pads we cannot recognise

Controllers are identified by their SDL GUID — the same identity the bundled controller database is keyed on —
so "which physical pad is player 2" survives being unplugged and moved to another USB port.

The database covers the mainstream: Xbox, DualSense and DualShock, Switch Pro, 8BitDo, and a couple of thousand
others. A device it does not cover is one whose buttons cannot be named, and guessing produces a config that is
silently wrong — the worst outcome, because the emulator then looks broken. So instead, at the launch that
would have configured it, EverythingBox says so:

> Unrecognized controller: *Frobnitz Arcade Pad*. EverythingBox can't tell which button is which, so it left it
> out of PCSX2's controller setup — map it in PCSX2's own controller settings.

No config is written for that pad. Other pads in the same session are seated normally.

Each device is named **once**, not on every launch. Anything the system reports as a joystick and SDL cannot
map lands here, and that includes things that are not really pads — wheels, flight sticks, analogue keyboards,
assorted HID oddities. Something permanently plugged in would otherwise put a warning in front of every game
you ever start, so the message appears the first time that device is seen and then stops. (The GUIDs already
mentioned are remembered in the app's own settings, not in any emulator's config.)

The database is `native/gamecontrollerdb.txt`. It is **bundled data refreshed per release, not code**, and it is
never fetched at runtime — see `native/gamecontrollerdb.provenance.txt` for the upstream commit each copy came
from and how to update it.

## What this does not do

- It does not choose *which* physical pad is player 2. Seats follow enumeration order; pinning a specific pad
  to a specific seat across a replug is not implemented yet.
- It does not turn multitap on for PCSX2 or DuckStation, so seats 3 and 4 there are written but inert until you
  do.
- It does not write hotkeys for Dolphin, Cemu, ares, melonDS or any emulator beyond the two in the table above.
- It never edits a config for an emulator whose switch is off, and never overwrites a player block that is
  already there.
