# Controller-Aware UI Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** When a controller is driving the app, hide the mouse cursor and render every on-screen hotkey hint as the controller button to press; when a mouse is driving it, restore both.

**Architecture:** One new authority, `InputMode`, holds which device is in use (`"pointer"` / `"pad"`) and the connected pad's brand, mirroring the existing `FormFactor` singleton. A pure companion, `PadGlyphs`, translates a hint string (`"Enter"`) through a UI verb (Confirm) to a RetroPad id, then through the pad's **live** binding to a brand-correct label (`"A"` / `"✕"` / `"B"`). `InputMode` is exposed to QML as `input` next to `form`, and `HelpSystem.qml` calls `input.chipFor()` for its chip text. The five browse verbs that were keyboard-only get real pad buttons in `MainWindow::pollMenuPad`. The cursor is hidden by `MainWindow` reacting to `InputMode::changed()` — the authority states the fact, the app reacts to it.

**Tech Stack:** C++17, Qt 6 (Core / Widgets / Quick), QML, SDL2 (behind `EVERYTHINGBOX_HAVE_SDL`), CMake, the repo's headless probe suite.

## Global Constraints

- **No AI attribution anywhere.** No `Co-Authored-By: Claude`, no "Generated with…", no tool name in a commit message, PR body or issue comment. Conventional prefixes (`feat:`, `fix:`, `docs:`, `refactor:`) still apply.
- **The working tree is SHARED with other concurrent sessions.** Never `git add -A`, never `git add .`, never `git commit -a`. Stage only the exact paths each task's commit step names. Unrelated modified files you did not touch must stay unstaged.
- **A version-bump hook runs on commit** and will add `native/CMakeLists.txt` and `native/src/main.cpp` version-line changes to your commit. That is expected; do not revert it and do not mention it in the message.
- **`native/tools/run-headless-probes.sh` is a CRLF file** and `native/CMakeLists.txt` contains a lone CR. Edit both with byte-exact tools (`python` with `newline=''`, or an `Edit` that preserves surrounding bytes). Do NOT normalise line endings — it breaks the suite silently. After any edit to the runner, verify with `bash -n native/tools/run-headless-probes.sh`.
- **A new probe must be registered in all THREE places or it silently never runs:** (1) `add_executable` + `target_link_libraries` in `native/CMakeLists.txt`, (2) the `for p in "probe_… …-OK"` loop near the end of `native/tools/run-headless-probes.sh`, (3) the `--target` list in the "Build probes" step of `.github/workflows/ci.yml`.
- **All UI goes through the nav kit** (`src/ui/nav`: NavRing / NavOverlay / Osk). Never `QDialog`, `QMessageBox`, `QInputDialog`, or a top-level window.
- **Build:** `cmake --build build --config Release --target <targets> -- /MP` from the repo root. A full rebuild is ~41s with `/MP`.
- **Probe idiom:** a probe prints `<NAME>-OK` on success and `<NAME>-FAIL <cond> (line N)` on stderr with a non-zero exit. Fixtures are computed independently of the code under test — never assert a value by calling the function that produces it.

## File Structure

| Path | Responsibility | Task |
|------|----------------|------|
| `native/src/input/PadGlyphs.h` / `.cpp` | **New.** Pure translation: hint string → verb → RetroPad id; SDL code + brand → button label. No Qt widgets, no SDL, no state. | 1 |
| `native/tools/probe_padglyph.cpp` | **New.** Exhaustive table probe over `PadGlyphs`. | 1 |
| `native/src/input/Gamepad.h` / `.cpp` | **Modify.** Add `brand(port)` in both the SDL and the inert branch. | 2 |
| `native/src/input/InputMode.h` / `.cpp` | **New.** The live authority: mode, brand, `chipFor()`, `notePad()`, `notePointer()`, `changed()`. QtCore only. | 2 |
| `native/tools/probe_inputmode.cpp` | **New.** Mode transitions, signal economy, live-binding translation. | 2 |
| `native/src/theme2/ThemeEngine.cpp`, `ThemedPanelHost.cpp`, `ThemePickerHost.cpp`, `ReaderChromeHost.cpp` | **Modify.** Register `input` next to `form`. | 3 |
| `native/src/theme2/qml/elements/HelpSystem.qml` | **Modify.** Chip text through `input.chipFor()` in pad mode. | 3 |
| `native/tools/probe_navqml.cpp` | **Modify.** New section: a real built view's help chip flips with the mode. | 3 |
| `native/src/ui/MainWindow.h` / `.cpp` | **Modify.** Grow the pad nav table to 12 rows, call `notePad`, react to `changed()` with the cursor override, install the pointer watch. | 4 |
| `native/src/ui/MainWindow.cpp` (copy) | **Modify.** Three key-naming strings take glyph placeholders. | 5 |
| `native/src/ui/nav/Osk.cpp` | **Modify.** Mode-aware footer. | 5 |
| `native/tools/run-headless-probes.sh` | **Modify.** Two probe rows + the bundled-theme chip gate. | 1, 2, 6 |
| `.github/workflows/ci.yml` | **Modify.** Two probe targets. | 1, 2 |
| `native/CMakeLists.txt` | **Modify.** Two probe targets; `InputMode`/`PadGlyphs` into the app target. | 1, 2 |

---

### Task 1: `PadGlyphs` — the pure translator

**Files:**
- Create: `native/src/input/PadGlyphs.h`
- Create: `native/src/input/PadGlyphs.cpp`
- Create: `native/tools/probe_padglyph.cpp`
- Modify: `native/CMakeLists.txt`
- Modify: `native/tools/run-headless-probes.sh`
- Modify: `.github/workflows/ci.yml`

**Interfaces:**
- Consumes: nothing.
- Produces, in `namespace padglyphs`:
  - `enum class Brand { Xbox, PlayStation, Switch, Generic }`
  - `enum class Verb { None, Confirm, Back, Details, Search, Filter, Playlist, Theme, Skip }`
  - `Brand brandFromName(const QString& name)` — `"xbox"|"playstation"|"switch"` → the enum; anything else → `Generic`.
  - `QString nameForBrand(Brand b)` — the inverse, lowercase.
  - `Verb verbForHint(const QString& hintKey)` — `"Enter"`→`Confirm`, `"Esc"`→`Back`, `"I"`→`Details`, `"/"`→`Search`, `"F"`→`Filter`, `"P"`→`Playlist`, `"T"`→`Theme`, `"S"`→`Skip`; everything else (arrow chips, third-party text) → `None`.
  - `int retroIdForVerb(Verb v)` — `Confirm`→0, `Back`→8, `Details`→9, `Search`→1, `Filter`→10, `Playlist`→11, `Theme`→2, `Skip`→1; `None`→-1.
  - `QString labelForSdlCode(int sdlCode, Brand b)` — `""` for an unbound/unknown code.
  - `QString chip(const QString& hintKey, Brand b, int sdlCode)` — the one call a consumer makes. Returns `hintKey` **unchanged** when the verb is `None` or `labelForSdlCode` is empty; otherwise the label.

**Reference — RetroPad ids** (libretro `RETRO_DEVICE_ID_JOYPAD_*`, used verbatim by `Gamepad`): `B`=0 (south), `Y`=1 (west), `SELECT`=2, `START`=3, `UP`=4, `DOWN`=5, `LEFT`=6, `RIGHT`=7, `A`=8 (east), `X`=9 (north), `L`=10, `R`=11, `L2`=12, `R2`=13, `L3`=14, `R3`=15.

**Reference — SDL codes** as stored by `Gamepad::binding()`: `0` A/south, `1` B/east, `2` X/west, `3` Y/north, `4` Back, `5` Guide, `6` Start, `7` LStick, `8` RStick, `9` LShoulder, `10` RShoulder, `11..14` D-pad up/down/left/right, `1000` `Gamepad::kTriggerLeft`, `1001` `Gamepad::kTriggerRight`, `-1` `Gamepad::kUnbound`.

- [ ] **Step 1: Write the failing probe**

Create `native/tools/probe_padglyph.cpp`:

```cpp
// Headless check of the pure pad-glyph translator (src/input/PadGlyphs) — the ONE place a keyboard hint
// string authored in a theme becomes the controller button a player is actually looking at. It is plain
// QtCore (no SDL, no widgets, no scene), so it runs under the offscreen QPA in CI and pins:
//
//   * verbForHint() — the eight hints the app owns map to their verb; an arrow chip and an arbitrary
//     third-party string map to None (which is what makes them pass through untranslated);
//   * retroIdForVerb() — each verb's RetroPad id, spelled out as literals here rather than read back from
//     the header, so a renumbering cannot pass by re-running the code under test;
//   * labelForSdlCode() — the full per-brand label table for every SDL code Gamepad can store, including
//     both trigger sentinels, and "" for the unbound sentinel;
//   * chip() — the composition rule: translate when the verb is known AND the code is bound, otherwise
//     return the caller's own string unchanged.
//
// Prints PADGLYPH-OK on success; any failure prints PADGLYPH-FAIL <cond> (line) and exits non-zero.
#include "PadGlyphs.h"

#include <QCoreApplication>
#include <QString>
#include <cstdio>

using namespace padglyphs;

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "PADGLYPH-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // 1. Brand names round-trip, and anything unrecognised is Generic (never a crash, never a guess).
    CHECK(brandFromName(QStringLiteral("xbox")) == Brand::Xbox);
    CHECK(brandFromName(QStringLiteral("playstation")) == Brand::PlayStation);
    CHECK(brandFromName(QStringLiteral("switch")) == Brand::Switch);
    CHECK(brandFromName(QStringLiteral("generic")) == Brand::Generic);
    CHECK(brandFromName(QStringLiteral("")) == Brand::Generic);
    CHECK(brandFromName(QStringLiteral("dreamcast")) == Brand::Generic);
    CHECK(nameForBrand(Brand::PlayStation) == QStringLiteral("playstation"));
    CHECK(nameForBrand(Brand::Generic) == QStringLiteral("generic"));

    // 2. The eight hints the app owns resolve to their verb.
    CHECK(verbForHint(QStringLiteral("Enter")) == Verb::Confirm);
    CHECK(verbForHint(QStringLiteral("Esc"))   == Verb::Back);
    CHECK(verbForHint(QStringLiteral("I"))     == Verb::Details);
    CHECK(verbForHint(QStringLiteral("/"))     == Verb::Search);
    CHECK(verbForHint(QStringLiteral("F"))     == Verb::Filter);
    CHECK(verbForHint(QStringLiteral("P"))     == Verb::Playlist);
    CHECK(verbForHint(QStringLiteral("T"))     == Verb::Theme);
    CHECK(verbForHint(QStringLiteral("S"))     == Verb::Skip);

    // 3. Everything else is None — the arrow chips every bundled theme ships, and a string only a
    //    third-party theme author knows about. None is what makes chip() hand the caller's text back.
    CHECK(verbForHint(QStringLiteral("\xe2\x86\x90")) == Verb::None);              // <-
    CHECK(verbForHint(QStringLiteral("\xe2\x86\x91\xe2\x86\x93")) == Verb::None);  // up/down
    CHECK(verbForHint(QStringLiteral("\xe2\x86\x90\xe2\x86\x92")) == Verb::None);  // left/right
    CHECK(verbForHint(QStringLiteral("Ctrl+Q")) == Verb::None);
    CHECK(verbForHint(QStringLiteral("")) == Verb::None);

    // 4. Verb -> RetroPad id. Literals, not a second call into the table.
    CHECK(retroIdForVerb(Verb::Confirm)  == 0);
    CHECK(retroIdForVerb(Verb::Search)   == 1);
    CHECK(retroIdForVerb(Verb::Skip)     == 1);   // same button, different surface
    CHECK(retroIdForVerb(Verb::Theme)    == 2);
    CHECK(retroIdForVerb(Verb::Back)     == 8);
    CHECK(retroIdForVerb(Verb::Details)  == 9);
    CHECK(retroIdForVerb(Verb::Filter)   == 10);
    CHECK(retroIdForVerb(Verb::Playlist) == 11);
    CHECK(retroIdForVerb(Verb::None)     == -1);

    // 5. The label table, hand-written per brand. Generic deliberately equals Xbox.
    struct Row { int code; const char* xbox; const char* ps; const char* sw; };
    static const Row rows[] = {
        {  0, "A",    "\xe2\x9c\x95", "B"    },   // south   / cross
        {  1, "B",    "\xe2\x97\x8b", "A"    },   // east    / circle
        {  2, "X",    "\xe2\x96\xa1", "Y"    },   // west    / square
        {  3, "Y",    "\xe2\x96\xb3", "X"    },   // north   / triangle
        {  4, "View", "Create",       "\xe2\x88\x92" },
        {  5, "Guide","PS",           "Home" },
        {  6, "Menu", "Options",      "+"    },
        {  7, "LS",   "L3",           "LS"   },
        {  8, "RS",   "R3",           "RS"   },
        {  9, "LB",   "L1",           "L"    },
        { 10, "RB",   "R1",           "R"    },
        { 11, "\xe2\x86\x91", "\xe2\x86\x91", "\xe2\x86\x91" },
        { 12, "\xe2\x86\x93", "\xe2\x86\x93", "\xe2\x86\x93" },
        { 13, "\xe2\x86\x90", "\xe2\x86\x90", "\xe2\x86\x90" },
        { 14, "\xe2\x86\x92", "\xe2\x86\x92", "\xe2\x86\x92" },
        { 1000, "LT", "L2", "ZL" },
        { 1001, "RT", "R2", "ZR" },
    };
    for (const Row& r : rows)
    {
        CHECK(labelForSdlCode(r.code, Brand::Xbox)        == QString::fromUtf8(r.xbox));
        CHECK(labelForSdlCode(r.code, Brand::Generic)     == QString::fromUtf8(r.xbox));
        CHECK(labelForSdlCode(r.code, Brand::PlayStation) == QString::fromUtf8(r.ps));
        CHECK(labelForSdlCode(r.code, Brand::Switch)      == QString::fromUtf8(r.sw));
    }

    // 6. Unbound and out-of-range codes have no label at all.
    CHECK(labelForSdlCode(-1, Brand::Xbox).isEmpty());
    CHECK(labelForSdlCode(99, Brand::Xbox).isEmpty());
    CHECK(labelForSdlCode(15, Brand::Switch).isEmpty());

    // 7. chip(): a known verb on a bound button renders the button.
    CHECK(chip(QStringLiteral("Enter"), Brand::Xbox, 0)        == QStringLiteral("A"));
    CHECK(chip(QStringLiteral("Enter"), Brand::PlayStation, 0) == QString::fromUtf8("\xe2\x9c\x95"));
    CHECK(chip(QStringLiteral("Esc"),   Brand::Switch, 1)      == QStringLiteral("A"));
    CHECK(chip(QStringLiteral("F"),     Brand::PlayStation, 9) == QStringLiteral("L1"));

    // 8. chip(): a remapped binding renders the button the user actually mapped, not the factory one.
    CHECK(chip(QStringLiteral("Enter"), Brand::Xbox, 3) == QStringLiteral("Y"));
    CHECK(chip(QStringLiteral("/"),     Brand::Xbox, 1000) == QStringLiteral("LT"));

    // 9. chip(): pass-through. An unknown hint keeps the theme author's own text; a known verb with
    //    nothing bound to it keeps the keyboard text rather than claiming a button that does not exist.
    CHECK(chip(QStringLiteral("\xe2\x86\x90\xe2\x86\x92"), Brand::Xbox, 0)
          == QString::fromUtf8("\xe2\x86\x90\xe2\x86\x92"));
    CHECK(chip(QStringLiteral("Ctrl+Q"), Brand::Xbox, 0) == QStringLiteral("Ctrl+Q"));
    CHECK(chip(QStringLiteral("Enter"),  Brand::Xbox, -1) == QStringLiteral("Enter"));
    CHECK(chip(QStringLiteral("T"),      Brand::Xbox, 99) == QStringLiteral("T"));

    if (failures == 0) std::printf("PADGLYPH-OK\n");
    else               std::fprintf(stderr, "PADGLYPH: %d check(s) failed\n", failures);
    return failures == 0 ? 0 : 1;
}
```

- [ ] **Step 2: Register the probe in all three places**

In `native/CMakeLists.txt`, insert immediately **after** the `probe_pad2key` block (find it with `grep -n "add_executable(probe_pad2key" native/CMakeLists.txt`):

```cmake
    # Pad-glyph translator (controller-aware UI): the PURE hint-string -> verb -> RetroPad -> brand-label
    # table. No SDL, no widgets, no scene — a theme's authored "Enter" becoming the button a player sees.
    # QtCore-only, so it links lean and runs under offscreen QPA.
    add_executable(probe_padglyph tools/probe_padglyph.cpp
        src/input/PadGlyphs.cpp src/input/PadGlyphs.h)
    target_include_directories(probe_padglyph PRIVATE src src/input)
    target_link_libraries(probe_padglyph PRIVATE Qt6::Core)
```

In `native/tools/run-headless-probes.sh` (**CRLF file** — use a byte-exact edit), add `"probe_padglyph PADGLYPH-OK"` to the `for p in …` list, immediately after `"probe_pad2key PAD2KEY-OK"`.

In `.github/workflows/ci.yml`, add `probe_padglyph` to the `cmake --build build --target …` list in the "Build probes" step, immediately after `probe_pad2key`.

- [ ] **Step 3: Run the probe build to verify it fails**

```bash
cmake --build build --config Release --target probe_padglyph -- /MP
```

Expected: FAIL — `Cannot open include file: 'PadGlyphs.h'` (the header does not exist yet).

- [ ] **Step 4: Write the header**

Create `native/src/input/PadGlyphs.h`:

```cpp
// Pure translation from an on-screen keyboard hint to the controller button a player should press.
//
// A theme authors its help bar as keyboard chips ("Enter", "Esc", "I", "/") and themes come from a public
// registry, so the app does not control what a chip says. This is the one place those strings become
// buttons: hint -> UI verb -> RetroPad id -> (the pad's LIVE binding, supplied by the caller) -> a label
// spelled the way the connected brand spells it. Everything here is a pure function over its arguments —
// no SDL, no widgets, no state — so probe_padglyph can pin the whole table headlessly.
//
// A string this file does not recognise is handed straight back. That is deliberate: a third-party theme's
// own chip is the author's text, and inventing a button for it would be a lie the player then presses.
#pragma once
#include <QString>

namespace padglyphs
{

// How the connected controller spells its buttons. Generic is Xbox's spelling — the de-facto lingua franca
// in frontends — and is what an unrecognised pad resolves to.
enum class Brand { Xbox, PlayStation, Switch, Generic };

// The app verbs a help chip can name. None means "not one of ours" (an arrow chip, a third-party string).
enum class Verb { None, Confirm, Back, Details, Search, Filter, Playlist, Theme, Skip };

Brand   brandFromName(const QString& name);   // "xbox"|"playstation"|"switch" -> enum; else Generic
QString nameForBrand(Brand b);                // the inverse, lowercase

// The hint strings the app owns. Search and Skip share a RetroPad button because they never appear on the
// same surface (Search is the browse UI, Skip is the video player).
Verb verbForHint(const QString& hintKey);

// The RetroPad id (RETRO_DEVICE_ID_JOYPAD_*) a verb rides. -1 for Verb::None.
int  retroIdForVerb(Verb v);

// An SDL_GameControllerButton code (as stored by Gamepad::binding) spelled for a brand. Empty for
// Gamepad::kUnbound and for anything outside the table — an empty label is how chip() knows to pass through.
QString labelForSdlCode(int sdlCode, Brand b);

// The one call a consumer makes. Returns `hintKey` unchanged when the verb is unknown or nothing is bound
// to it; otherwise the brand-correct label for `sdlCode`.
QString chip(const QString& hintKey, Brand b, int sdlCode);

} // namespace padglyphs
```

- [ ] **Step 5: Write the implementation**

Create `native/src/input/PadGlyphs.cpp`:

```cpp
#include "PadGlyphs.h"

namespace padglyphs
{

Brand brandFromName(const QString& name)
{
    if (name == QLatin1String("xbox"))        return Brand::Xbox;
    if (name == QLatin1String("playstation")) return Brand::PlayStation;
    if (name == QLatin1String("switch"))      return Brand::Switch;
    return Brand::Generic;
}

QString nameForBrand(Brand b)
{
    switch (b)
    {
    case Brand::Xbox:        return QStringLiteral("xbox");
    case Brand::PlayStation: return QStringLiteral("playstation");
    case Brand::Switch:      return QStringLiteral("switch");
    case Brand::Generic:     break;
    }
    return QStringLiteral("generic");
}

Verb verbForHint(const QString& hintKey)
{
    if (hintKey == QLatin1String("Enter")) return Verb::Confirm;
    if (hintKey == QLatin1String("Esc"))   return Verb::Back;
    if (hintKey == QLatin1String("I"))     return Verb::Details;
    if (hintKey == QLatin1String("/"))     return Verb::Search;
    if (hintKey == QLatin1String("F"))     return Verb::Filter;
    if (hintKey == QLatin1String("P"))     return Verb::Playlist;
    if (hintKey == QLatin1String("T"))     return Verb::Theme;
    if (hintKey == QLatin1String("S"))     return Verb::Skip;
    return Verb::None;   // arrow chips and third-party text: the caller's own string survives
}

int retroIdForVerb(Verb v)
{
    // RETRO_DEVICE_ID_JOYPAD_*: B=0 (south) Y=1 (west) SELECT=2 START=3 A=8 (east) X=9 (north) L=10 R=11.
    switch (v)
    {
    case Verb::Confirm:  return 0;
    case Verb::Search:   return 1;
    case Verb::Skip:     return 1;   // player surface; never on screen at the same time as Search
    case Verb::Theme:    return 2;
    case Verb::Back:     return 8;
    case Verb::Details:  return 9;
    case Verb::Filter:   return 10;
    case Verb::Playlist: return 11;
    case Verb::None:     break;
    }
    return -1;
}

QString labelForSdlCode(int sdlCode, Brand b)
{
    const bool ps = (b == Brand::PlayStation);
    const bool sw = (b == Brand::Switch);
    switch (sdlCode)
    {
    case 0:  return ps ? QString::fromUtf8("\xe2\x9c\x95") : sw ? QStringLiteral("B") : QStringLiteral("A");
    case 1:  return ps ? QString::fromUtf8("\xe2\x97\x8b") : sw ? QStringLiteral("A") : QStringLiteral("B");
    case 2:  return ps ? QString::fromUtf8("\xe2\x96\xa1") : sw ? QStringLiteral("Y") : QStringLiteral("X");
    case 3:  return ps ? QString::fromUtf8("\xe2\x96\xb3") : sw ? QStringLiteral("X") : QStringLiteral("Y");
    case 4:  return ps ? QStringLiteral("Create") : sw ? QString::fromUtf8("\xe2\x88\x92") : QStringLiteral("View");
    case 5:  return ps ? QStringLiteral("PS")      : sw ? QStringLiteral("Home") : QStringLiteral("Guide");
    case 6:  return ps ? QStringLiteral("Options") : sw ? QStringLiteral("+")    : QStringLiteral("Menu");
    case 7:  return ps ? QStringLiteral("L3") : QStringLiteral("LS");
    case 8:  return ps ? QStringLiteral("R3") : QStringLiteral("RS");
    case 9:  return ps ? QStringLiteral("L1") : sw ? QStringLiteral("L") : QStringLiteral("LB");
    case 10: return ps ? QStringLiteral("R1") : sw ? QStringLiteral("R") : QStringLiteral("RB");
    case 11: return QString::fromUtf8("\xe2\x86\x91");
    case 12: return QString::fromUtf8("\xe2\x86\x93");
    case 13: return QString::fromUtf8("\xe2\x86\x90");
    case 14: return QString::fromUtf8("\xe2\x86\x92");
    case 1000: return ps ? QStringLiteral("L2") : sw ? QStringLiteral("ZL") : QStringLiteral("LT");
    case 1001: return ps ? QStringLiteral("R2") : sw ? QStringLiteral("ZR") : QStringLiteral("RT");
    default: break;
    }
    return QString();   // Gamepad::kUnbound, and anything SDL never hands us
}

QString chip(const QString& hintKey, Brand b, int sdlCode)
{
    if (verbForHint(hintKey) == Verb::None) return hintKey;
    const QString label = labelForSdlCode(sdlCode, b);
    return label.isEmpty() ? hintKey : label;
}

} // namespace padglyphs
```

- [ ] **Step 6: Build and run the probe**

```bash
cmake --build build --config Release --target probe_padglyph -- /MP
```

Then run it (the runner finds probes in `build/` or `build/Release/`):

```bash
./build/Release/probe_padglyph.exe
```

Expected: prints `PADGLYPH-OK`, exit code 0.

- [ ] **Step 7: Verify the runner and CI list parse**

```bash
bash -n native/tools/run-headless-probes.sh
```

Expected: no output, exit 0. Then confirm the three registrations landed:

```bash
grep -c probe_padglyph native/CMakeLists.txt native/tools/run-headless-probes.sh .github/workflows/ci.yml
```

Expected: `1` from CMakeLists (the `add_executable` line; `target_*` lines also match, so ≥1 is fine — confirm each file reports a non-zero count).

- [ ] **Step 8: Commit**

```bash
git add native/src/input/PadGlyphs.h native/src/input/PadGlyphs.cpp native/tools/probe_padglyph.cpp native/CMakeLists.txt native/tools/run-headless-probes.sh .github/workflows/ci.yml
git commit -m "feat: translate keyboard hint chips to controller button labels"
```

---

### Task 2: `Gamepad::brand()` and the `InputMode` authority

**Files:**
- Modify: `native/src/input/Gamepad.h` (add `brand()` to the public API)
- Modify: `native/src/input/Gamepad.cpp` (implement in BOTH the SDL and the inert branch)
- Create: `native/src/input/InputMode.h`
- Create: `native/src/input/InputMode.cpp`
- Create: `native/tools/probe_inputmode.cpp`
- Modify: `native/CMakeLists.txt`, `native/tools/run-headless-probes.sh`, `.github/workflows/ci.yml`

**Interfaces:**
- Consumes: `padglyphs::chip`, `padglyphs::brandFromName`, `padglyphs::verbForHint`, `padglyphs::retroIdForVerb` (Task 1); `Gamepad::binding(port, retroId)`, `Gamepad::kUnbound` (existing).
- Produces:
  - `std::string Gamepad::brand(unsigned port) const` — `"xbox"|"playstation"|"switch"|"generic"`.
  - `class InputMode : public QObject` with `static InputMode& instance()`, `Q_PROPERTY(QString mode READ modeName NOTIFY changed)`, `Q_PROPERTY(QString brand READ brand NOTIFY changed)`, `Q_INVOKABLE QString chipFor(const QString& hintKey) const`, `void setPad(Gamepad* pad)`, `void notePad(unsigned port)`, `void notePointer()`, `bool padMode() const`, signal `void changed()`.

**Note on the cursor:** the spec assigns `setOverrideCursor` to the mode transition. `InputMode` deliberately does **not** touch the cursor — it stays QtCore-only so it can be probed exactly like `FormFactor`. `MainWindow` reacts to `changed()` and owns the cursor (Task 4).

- [ ] **Step 1: Write the failing probe**

Create `native/tools/probe_inputmode.cpp`:

```cpp
// Headless check of the input-mode authority (src/input/InputMode) — the ONE answer to "is a controller or
// a mouse driving this app right now", and the object every themed surface reads as `input`. It is plain
// QtCore (no widgets, no scene) and links Gamepad WITHOUT SDL (EVERYTHINGBOX_HAVE_SDL is set only on the
// app target), so the inert Gamepad still serves real per-port BINDINGS out of Settings — which is exactly
// the surface this probe needs. Pins:
//
//   * the app starts in pointer mode with a generic brand, even though a pad may be attached: the mode
//     follows USE, not presence;
//   * notePad()/notePointer() flip the mode and emit changed() exactly once per REAL change — a repeat of
//     the current mode is silent, or every polled controller frame would re-run every QML binding;
//   * chipFor() resolves through the pad's LIVE binding, so a remapped button renders the button the user
//     actually mapped, and an unbound verb falls back to the keyboard text;
//   * with no pad attached at all, chipFor() still answers from the factory bindings rather than blanking.
//
// Prints INPUTMODE-OK on success; any failure prints INPUTMODE-FAIL <cond> (line) and exits non-zero.
//
// Isolation (issue #42): AppPaths::dataDir() is this process's own scratch dir, so the everythingbox.ini
// that Settings opens starts empty — the factory-binding assertions below are only defaults while nothing
// has written the keys.
#include "InputMode.h"
#include "Gamepad.h"
#include "PadGlyphs.h"
#include "Settings.h"

#include <QCoreApplication>
#include <QSignalSpy>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "INPUTMODE-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    InputMode& im = InputMode::instance();

    // 1. Startup is pointer mode with a generic brand — nothing has been used yet.
    CHECK(im.modeName() == QStringLiteral("pointer"));
    CHECK(im.padMode() == false);
    CHECK(im.brand() == QStringLiteral("generic"));

    // 2. With no pad set, chipFor still answers from the FACTORY bindings (Xbox spelling): a help bar on a
    //    machine whose pad has not been opened yet must not render blanks.
    CHECK(im.chipFor(QStringLiteral("Enter")) == QStringLiteral("A"));
    CHECK(im.chipFor(QStringLiteral("Esc"))   == QStringLiteral("B"));

    // 3. notePad flips the mode and fires changed() ONCE; a second notePad on the same port is silent.
    {
        QSignalSpy spy(&im, &InputMode::changed);
        im.notePad(0);
        CHECK(im.modeName() == QStringLiteral("pad"));
        CHECK(im.padMode() == true);
        CHECK(spy.count() == 1);
        im.notePad(0);
        CHECK(spy.count() == 1);   // still 1: a polled pad must not re-run every QML binding
    }

    // 4. notePointer flips back, once; a repeat is silent.
    {
        QSignalSpy spy(&im, &InputMode::changed);
        im.notePointer();
        CHECK(im.modeName() == QStringLiteral("pointer"));
        CHECK(spy.count() == 1);
        im.notePointer();
        CHECK(spy.count() == 1);
    }

    // 5. With a pad attached, every hint the app owns renders its factory button (Xbox spelling, because a
    //    Gamepad built without SDL reports no controller type). Expected labels are hand-written literals.
    Gamepad pad;
    im.setPad(&pad);
    im.notePad(0);
    CHECK(im.chipFor(QStringLiteral("Enter")) == QStringLiteral("A"));    // RetroPad B  -> SDL 0
    CHECK(im.chipFor(QStringLiteral("Esc"))   == QStringLiteral("B"));    // RetroPad A  -> SDL 1
    CHECK(im.chipFor(QStringLiteral("I"))     == QStringLiteral("Y"));    // RetroPad X  -> SDL 3
    CHECK(im.chipFor(QStringLiteral("/"))     == QStringLiteral("X"));    // RetroPad Y  -> SDL 2
    CHECK(im.chipFor(QStringLiteral("S"))     == QStringLiteral("X"));    // same button, player surface
    CHECK(im.chipFor(QStringLiteral("F"))     == QStringLiteral("LB"));   // RetroPad L  -> SDL 9
    CHECK(im.chipFor(QStringLiteral("P"))     == QStringLiteral("RB"));   // RetroPad R  -> SDL 10
    CHECK(im.chipFor(QStringLiteral("T"))     == QStringLiteral("View")); // RetroPad SELECT -> SDL 4

    // 6. Pass-through survives the live path: arrow chips and third-party text keep the theme's own string.
    CHECK(im.chipFor(QString::fromUtf8("\xe2\x86\x90\xe2\x86\x92"))
          == QString::fromUtf8("\xe2\x86\x90\xe2\x86\x92"));
    CHECK(im.chipFor(QStringLiteral("Ctrl+Q")) == QStringLiteral("Ctrl+Q"));

    // 7. A REMAPPED binding renders the button the user actually mapped. Written through Settings and
    //    reloaded the way the input panel does it, not by poking InputMode.
    Settings::setPadBinding(0, /*RETRO_DEVICE_ID_JOYPAD_B*/ 0, /*SDL Y (north)*/ 3);
    pad.reloadMapping();
    CHECK(im.chipFor(QStringLiteral("Enter")) == QStringLiteral("Y"));

    // 8. An UNBOUND verb keeps the keyboard text — the bar never claims a button that does not exist.
    Settings::setPadBinding(0, /*RETRO_DEVICE_ID_JOYPAD_Y (west)*/ 1, Gamepad::kUnbound);
    pad.reloadMapping();
    CHECK(im.chipFor(QStringLiteral("/")) == QStringLiteral("/"));

    // 9. The brand is read from the pad on the port that last sent input, and an unrecognised pad (which is
    //    every pad in a no-SDL build) is generic rather than a guess.
    CHECK(im.brand() == QStringLiteral("generic"));

    if (failures == 0) std::printf("INPUTMODE-OK\n");
    else               std::fprintf(stderr, "INPUTMODE: %d check(s) failed\n", failures);
    return failures == 0 ? 0 : 1;
}
```

- [ ] **Step 2: Register the probe in all three places**

In `native/CMakeLists.txt`, immediately after the `probe_padglyph` block from Task 1:

```cmake
    # Input-mode authority (controller-aware UI): pointer-vs-pad state, its signal economy, and chipFor()
    # resolving through the pad's LIVE binding. Links Gamepad WITHOUT SDL (that define is set only on the
    # app target), so the inert Gamepad still serves real bindings out of Settings. QtCore + Qt6::Test for
    # QSignalSpy — no Quick, no Widgets, so it runs under offscreen QPA.
    add_executable(probe_inputmode tools/probe_inputmode.cpp
        src/input/InputMode.cpp src/input/InputMode.h
        src/input/PadGlyphs.cpp src/input/PadGlyphs.h
        src/input/Gamepad.cpp   src/input/Gamepad.h
        src/core/Settings.cpp   src/core/Settings.h
        src/core/LifecyclePolicy.h)
    target_include_directories(probe_inputmode PRIVATE src src/input src/core)
    target_link_libraries(probe_inputmode PRIVATE Qt6::Core Qt6::Test)
```

In `native/tools/run-headless-probes.sh` (**CRLF**), add `"probe_inputmode INPUTMODE-OK"` to the `for p in …` list, immediately after `"probe_padglyph PADGLYPH-OK"`.

In `.github/workflows/ci.yml`, add `probe_inputmode` after `probe_padglyph`.

- [ ] **Step 3: Run the build to verify it fails**

```bash
cmake --build build --config Release --target probe_inputmode -- /MP
```

Expected: FAIL — `Cannot open include file: 'InputMode.h'`.

- [ ] **Step 4: Add `Gamepad::brand()`**

In `native/src/input/Gamepad.h`, add to the public section immediately after the `describeControllers()` declaration:

```cpp
    // How the controller on this port spells its buttons: "xbox" | "playstation" | "switch" | "generic".
    // Derived from SDL_GameControllerGetType; an unrecognised or absent pad is "generic" (Xbox spelling,
    // the de-facto lingua franca in frontends). Used by InputMode to label on-screen hints.
    std::string brand(unsigned port = 0) const;
```

In `native/src/input/Gamepad.cpp`, inside the `#ifdef EVERYTHINGBOX_HAVE_SDL` branch, immediately after the `describeControllers()` implementation:

```cpp
std::string Gamepad::brand(unsigned port) const
{
    if (!initialized_ || port >= unsigned(kMaxPlayers) || !slots_[port]) return "generic";
    switch (SDL_GameControllerGetType(static_cast<SDL_GameController*>(slots_[port])))
    {
    case SDL_CONTROLLER_TYPE_XBOX360:
    case SDL_CONTROLLER_TYPE_XBOXONE:
        return "xbox";
    case SDL_CONTROLLER_TYPE_PS3:
    case SDL_CONTROLLER_TYPE_PS4:
    case SDL_CONTROLLER_TYPE_PS5:
        return "playstation";
    case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO:
    case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
    case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
    case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
        return "switch";
    default:
        return "generic";   // Luna, Stadia, Shield, virtual, unknown third-party
    }
}
```

And in the `#else // ---- SDL2 not compiled in: gamepad is inert ----` branch, immediately after the inert `describeControllers()`:

```cpp
std::string Gamepad::brand(unsigned) const { return "generic"; }
```

- [ ] **Step 5: Write `InputMode.h`**

Create `native/src/input/InputMode.h`:

```cpp
// The ONE authority on which device is driving the app right now, and the object every themed surface reads
// as `input` (registered next to `form`, exactly like FormFactor). Two facts live here:
//
//   * mode — "pointer" or "pad". A controller press puts it in pad mode; a REAL mouse movement puts it back.
//     A keypress changes nothing: a keyboard on a couch is not a mouse. Startup is pointer mode even with a
//     pad attached, because the mode follows USE, not presence.
//   * brand — how the pad on the port that last sent input spells its buttons, which is what chipFor() needs.
//
// QtCore only, on purpose: no cursor code and no widgets live here, so probe_inputmode can pin the whole
// contract headlessly the way probe_formfactor pins FormFactor's. MainWindow reacts to changed() and owns
// the cursor; this object only states the fact.
//
// SIGNAL ECONOMY MATTERS. changed() is a QML binding's NOTIFY: every themed help chip re-evaluates on it.
// notePad() is called from the controller poll timer, so it MUST be silent when the mode is already pad, or
// the whole scene re-binds sixty times a second.
#pragma once
#include <QObject>
#include <QString>

class Gamepad;

class InputMode : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString mode  READ modeName NOTIFY changed)
    Q_PROPERTY(QString brand READ brand    NOTIFY changed)
public:
    static InputMode& instance();

    QString modeName() const;             // "pointer" | "pad"
    bool    padMode() const { return pad_; }
    QString brand() const;                // "xbox" | "playstation" | "switch" | "generic"

    // Translate one help-bar chip. Resolves the hint's verb to a RetroPad id, asks the pad for that id's
    // LIVE binding (so a remap shows the button the user actually mapped), and spells it for the brand.
    // Returns `hintKey` unchanged when the hint is not one of ours or nothing is bound to it. Always
    // translates — the CALLER decides when to ask (HelpSystem only asks in pad mode).
    Q_INVOKABLE QString chipFor(const QString& hintKey) const;

    // The app's one Gamepad, BORROWED — not owned; must outlive this object's use. Null is fine: chipFor
    // then answers from the factory bindings.
    void setPad(Gamepad* pad) { gamepad_ = pad; }

    void notePad(unsigned port);   // a controller press happened on this port
    void notePointer();            // a real mouse movement happened

signals:
    void changed();

private:
    InputMode() = default;
    Gamepad* gamepad_ = nullptr;   // borrowed
    bool     pad_ = false;
    unsigned port_ = 0;
};
```

- [ ] **Step 6: Write `InputMode.cpp`**

Create `native/src/input/InputMode.cpp`:

```cpp
#include "InputMode.h"
#include "Gamepad.h"
#include "PadGlyphs.h"

InputMode& InputMode::instance()
{
    static InputMode s;
    return s;
}

QString InputMode::modeName() const
{
    return pad_ ? QStringLiteral("pad") : QStringLiteral("pointer");
}

QString InputMode::brand() const
{
    if (!gamepad_) return QStringLiteral("generic");
    return QString::fromStdString(gamepad_->brand(port_));
}

QString InputMode::chipFor(const QString& hintKey) const
{
    const padglyphs::Verb v = padglyphs::verbForHint(hintKey);
    if (v == padglyphs::Verb::None) return hintKey;
    const int retroId = padglyphs::retroIdForVerb(v);
    // With no pad opened yet, the factory mapping is still the honest answer: it is what that button will do
    // the moment a controller is plugged in, and blanking the chip would be worse than being one remap stale.
    const int sdlCode = gamepad_ ? gamepad_->binding(port_, unsigned(retroId))
                                 : Gamepad::defaultBinding(unsigned(retroId));
    return padglyphs::chip(hintKey, padglyphs::brandFromName(brand()), sdlCode);
}

void InputMode::notePad(unsigned port)
{
    // The port matters even when the mode does not change: the brand is read from whichever pad is driving.
    const bool portChanged = (port != port_);
    port_ = port;
    if (pad_ && !portChanged) return;   // already in pad mode on this port — stay silent (see the header)
    pad_ = true;
    emit changed();
}

void InputMode::notePointer()
{
    if (!pad_) return;
    pad_ = false;
    emit changed();
}
```

- [ ] **Step 7: Build and run the probe**

```bash
cmake --build build --config Release --target probe_inputmode -- /MP
```

Then:

```bash
./build/Release/probe_inputmode.exe
```

Expected: prints `INPUTMODE-OK`, exit code 0.

- [ ] **Step 8: Add both new sources to the app target**

In `native/CMakeLists.txt`, find the app target's source list (`grep -n "src/input/Gamepad.cpp" native/CMakeLists.txt` — the occurrence inside the `qt_add_executable(everythingbox …)` / app `target_sources` block, not a probe block) and add on the following line:

```cmake
        src/input/InputMode.cpp  src/input/InputMode.h
        src/input/PadGlyphs.cpp  src/input/PadGlyphs.h
```

Then confirm the app still links:

```bash
cmake --build build --config Release --target everythingbox -- /MP
```

Expected: build succeeds, no new warnings mentioning `InputMode` or `PadGlyphs`.

- [ ] **Step 9: Verify the runner parses**

```bash
bash -n native/tools/run-headless-probes.sh
```

Expected: no output, exit 0.

- [ ] **Step 10: Commit**

```bash
git add native/src/input/InputMode.h native/src/input/InputMode.cpp native/src/input/Gamepad.h native/src/input/Gamepad.cpp native/tools/probe_inputmode.cpp native/CMakeLists.txt native/tools/run-headless-probes.sh .github/workflows/ci.yml
git commit -m "feat: add the input-mode authority and per-brand controller identification"
```

---

### Task 3: Expose `input` to QML and swap the help-bar chips

**Files:**
- Modify: `native/src/theme2/ThemeEngine.cpp`
- Modify: `native/src/theme2/ThemedPanelHost.cpp`
- Modify: `native/src/theme2/ThemePickerHost.cpp`
- Modify: `native/src/theme2/ReaderChromeHost.cpp`
- Modify: `native/src/theme2/qml/elements/HelpSystem.qml`
- Modify: `native/tools/probe_navqml.cpp`

**Interfaces:**
- Consumes: `InputMode::instance()`, `InputMode::chipFor`, `InputMode::mode` (Task 2).
- Produces: the QML context property `input` on every themed surface; `HelpSystem.qml` chips that follow it.

**Why all four hosts:** each is a separate `QQuickWidget` with its own root context. A surface whose context lacks `input` renders its chips exactly as today (the QML is `typeof`-guarded), so a missed host is a silent half-feature — the same failure mode the nav kit's three-place probe rule exists to prevent.

- [ ] **Step 1: Write the failing probe section**

In `native/tools/probe_navqml.cpp`, append a new section function before `main`, and call it from `main` after the existing final section. Find the last section with `grep -n "^// ---- §2" native/tools/probe_navqml.cpp | tail -3` and follow that file's own numbering (use the next free number; the text below says `§N` — replace it with that number in both the comment and the FAIL strings).

```cpp
// ---- §N: the help bar follows the input mode -----------------------------------------------------------
// The claim: a REAL themed view built by ThemeEngine::buildView renders its helpsystem chips as keyboard
// text while a pointer is driving, and as brand-correct controller buttons the moment a pad is. This is the
// end-to-end cover for the chip swap — probe_padglyph pins the table and probe_inputmode pins the authority,
// but only this section proves the scene actually asks. Restores pointer mode on the way out so any later
// section sees the state it expects.
static void sectionHelpMode()
{
    // A scratch theme whose home view is nothing but a help bar, so the chip is trivially findable.
    QTemporaryDir dir;
    CHECKX(dir.isValid(), "helpmode: scratch dir");
    const QString themeDir = dir.path() + QStringLiteral("/HelpProbe");
    QDir().mkpath(themeDir);
    QFile f(themeDir + QStringLiteral("/theme.json"));
    CHECKX(f.open(QIODevice::WriteOnly | QIODevice::Text), "helpmode: theme.json open");
    f.write(R"({
      "name": "HelpProbe",
      "views": { "home": { "background": "#101014", "elements": [
        { "type": "helpsystem", "id": "help", "pos": [0.5, 0.9], "size": [1, 0.05], "origin": [0.5, 0.5],
          "color": "#FFFFFF", "fontSize": 0.03,
          "entries": [ { "button": "Enter", "label": "Open" },
                       { "button": "Esc",   "label": "Back" },
                       { "button": "\u2190\u2192", "label": "Move" } ] }
      ] } }
    })");
    f.close();

    InputMode::instance().notePointer();          // start from the documented default

    QWidget holder;
    QQuickWidget* view = ThemeEngine::buildView(&holder, themeDir, /*items*/ {});
    CHECKX(view != nullptr, "helpmode: buildView");
    QQuickItem* root = ThemeEngine::rootItem(view);
    CHECKX(root != nullptr, "helpmode: root");

    // Collect the chip Texts in author order. HelpSystem draws each chip's button into a Text inside a
    // Rectangle inside a Row; the visual walk (not findChildren, which follows QObject parentage) is the
    // same one §22 uses.
    auto chipTexts = [](QQuickItem* r) {
        QStringList out;
        std::function<void(QQuickItem*)> walk = [&](QQuickItem* it) {
            if (!it) return;
            const QVariant t = it->property("text");
            if (t.isValid() && it->metaObject()->className() == QLatin1String("QQuickText"))
                out << t.toString();
            for (QQuickItem* c : it->childItems()) walk(c);
        };
        walk(r);
        return out;
    };

    QStringList before = chipTexts(root);
    CHECKX(before.contains(QStringLiteral("Enter")), "helpmode: keyboard chip in pointer mode");
    CHECKX(before.contains(QStringLiteral("Esc")),   "helpmode: Esc chip in pointer mode");

    // Flip to pad mode. No Gamepad is attached in this probe, so chipFor answers from the factory bindings:
    // Enter -> A, Esc -> B. The arrow chip is not one of ours and must survive untouched.
    InputMode::instance().notePad(0);
    QCoreApplication::processEvents();            // let the bindings re-evaluate on changed()
    QStringList after = chipTexts(root);
    CHECKX(after.contains(QStringLiteral("A")),  "helpmode: Confirm chip became A");
    CHECKX(after.contains(QStringLiteral("B")),  "helpmode: Back chip became B");
    CHECKX(!after.contains(QStringLiteral("Enter")), "helpmode: keyboard chip gone in pad mode");
    CHECKX(after.contains(QString::fromUtf8("\xe2\x86\x90\xe2\x86\x92")),
           "helpmode: arrow chip untouched");

    // The LABEL half never changes — the verb is the same verb on either device.
    CHECKX(after.contains(QStringLiteral("Open")), "helpmode: label survives");

    InputMode::instance().notePointer();          // leave the state as later sections expect
}
```

Use whatever failure macro `probe_navqml.cpp` already defines (find it with `grep -n "define CHECK" native/tools/probe_navqml.cpp`) — the `CHECKX(cond, "text")` above is a placeholder for that file's own idiom. Add `#include "input/InputMode.h"` to its include block, and `probe_navqml`'s CMake target needs `src/input/InputMode.cpp`, `src/input/PadGlyphs.cpp`, `src/input/Gamepad.cpp` added to its source list if they are not already reachable through the sources it links.

- [ ] **Step 2: Run the probe to verify it fails**

```bash
cmake --build build --config Release --target probe_navqml -- /MP && ./build/Release/probe_navqml.exe
```

Expected: FAIL — `helpmode: Confirm chip became A` (the chips are still keyboard text; nothing registers `input` and `HelpSystem.qml` never asks).

- [ ] **Step 3: Register `input` in all four hosts**

`native/src/theme2/ThemeEngine.cpp` — find the line
`qv->rootContext()->setContextProperty(QStringLiteral("form"), &FormFactor::instance());`
and add immediately after it:

```cpp
    // The input-mode authority (controller-aware UI): the help bar reads `input.mode` to decide whether its
    // chips name keys or controller buttons, and `input.chipFor()` to spell them. Singleton, like `form`, so
    // it is not parented here; context properties must precede setSource.
    qv->rootContext()->setContextProperty(QStringLiteral("input"), &InputMode::instance());
```

Add `#include "../input/InputMode.h"` to that file's include block.

`native/src/theme2/ThemedPanelHost.cpp` — after its `setContextProperty(QStringLiteral("form"), …)` line:

```cpp
    view_->rootContext()->setContextProperty(QStringLiteral("input"), &InputMode::instance());
```

`native/src/theme2/ThemePickerHost.cpp` — after its `form` line, same one-liner with `view_->`.

`native/src/theme2/ReaderChromeHost.cpp` — after its `form` line, same one-liner with `qv->`.

Add `#include "../input/InputMode.h"` to each of the three.

- [ ] **Step 4: Swap the chip text in `HelpSystem.qml`**

In `native/src/theme2/qml/elements/HelpSystem.qml`, replace the chip `Text`'s `text:` line

```qml
                        text: modelData.button ? modelData.button : ""
```

with

```qml
                        // Controller-aware chips: while a pad is driving, the button the player is looking
                        // at replaces the key they would have typed. Reading `input.mode` here is what
                        // subscribes this binding to InputMode::changed(), so a brand change re-spells the
                        // chip too. typeof-guarded like every `form` consumer, so a fixture loaded without
                        // `input` renders exactly as before. A hint InputMode does not own comes back
                        // unchanged — a third-party theme's own chip stays the author's text.
                        text: padMode ? input.chipFor(modelData.button ? modelData.button : "")
                                      : (modelData.button ? modelData.button : "")
```

and add, next to the element's other `readonly property` declarations near the top (beside `fg`/`chip`/`fs`):

```qml
    readonly property bool padMode: (typeof input !== "undefined") && input && input.mode === "pad"
```

- [ ] **Step 5: Run the probe to verify it passes**

```bash
cmake --build build --config Release --target probe_navqml -- /MP && ./build/Release/probe_navqml.exe
```

Expected: prints `NAVQML-OK`, exit code 0.

- [ ] **Step 6: Verify no themed regression**

```bash
cmake --build build --config Release --target probe_themeview probe_theme2 probe_nav -- /MP
./build/Release/probe_themeview.exe && ./build/Release/probe_nav.exe
```

Expected: `THEMEVIEW-OK` and `NAV-OK`, both exit 0.

- [ ] **Step 7: Commit**

```bash
git add native/src/theme2/ThemeEngine.cpp native/src/theme2/ThemedPanelHost.cpp native/src/theme2/ThemePickerHost.cpp native/src/theme2/ReaderChromeHost.cpp native/src/theme2/qml/elements/HelpSystem.qml native/tools/probe_navqml.cpp native/CMakeLists.txt
git commit -m "feat: render help-bar chips as controller buttons while a pad is driving"
```

---

### Task 4: Real pad buttons for the browse verbs, and the cursor

**Files:**
- Modify: `native/src/ui/MainWindow.h` (grow `padPrev_` / `padNext_`; add the cursor flag)
- Modify: `native/src/ui/MainWindow.cpp` (`pollMenuPad` table, `notePad`, the pointer watch, the cursor reaction)

**Interfaces:**
- Consumes: `InputMode::instance()`, `InputMode::notePad`, `InputMode::notePointer`, `InputMode::setPad`, `InputMode::padMode`, `InputMode::changed` (Task 2).
- Produces: no new API. Behavioural: the pad's north/west/L/R/Select send `Key_I` / `Key_Slash` / `Key_F` / `Key_P` / `Key_T` in the browse UI, and north/west send `Key_I` / `Key_S` in the player.

**This task has no probe.** `pollMenuPad` needs SDL, a window and a focused app; the suite cannot see any of it (CONTRIBUTING is explicit that the probe suite cannot see focus or what a user experiences). Its verification is the live `EB_UITEST` pass in Task 6. Keep the change small and obviously correct.

- [ ] **Step 1: Grow the pad edge-state arrays**

In `native/src/ui/MainWindow.h`, replace

```cpp
    bool    padPrev_[8] = { false };  // per-nav-input: was it held last tick (edge detection)
    qint64  padNext_[8] = { 0 };      // per-nav-input: tick at which a held direction may repeat again
```

with

```cpp
    // 12 rows: the seven original nav inputs plus north/west/L/R/Select (controller-aware UI). Indices are
    // FIXED — a row that is inert on the current surface still records its held state, so a button held
    // across a surface change cannot fire a spurious press on arrival.
    bool    padPrev_[12] = { false }; // per-nav-input: was it held last tick (edge detection)
    qint64  padNext_[12] = { 0 };     // per-nav-input: tick at which a held direction may repeat again
    bool    padCursorHidden_ = false; // we own an active QApplication override cursor
```

- [ ] **Step 2: Extend the RetroPad id constants**

In `native/src/ui/MainWindow.cpp`, replace

```cpp
namespace { constexpr int PAD_B = 0, PAD_START = 3, PAD_UP = 4, PAD_DOWN = 5, PAD_LEFT = 6, PAD_RIGHT = 7, PAD_A = 8; }
```

with

```cpp
namespace {
constexpr int PAD_B = 0, PAD_Y = 1, PAD_SELECT = 2, PAD_START = 3, PAD_UP = 4, PAD_DOWN = 5,
              PAD_LEFT = 6, PAD_RIGHT = 7, PAD_A = 8, PAD_X = 9, PAD_L = 10, PAD_R = 11;
}
```

- [ ] **Step 3: Grow the nav table and note the input**

In `MainWindow::pollMenuPad`, replace the table declaration

```cpp
    struct Nav { int id; int key; bool repeat; };
    static const Nav navs[] = {
        { PAD_UP,    Qt::Key_Up,        true  }, { PAD_DOWN,  Qt::Key_Down,      true  },
        { PAD_LEFT,  Qt::Key_Left,      true  }, { PAD_RIGHT, Qt::Key_Right,     true  },
        { PAD_B,     Qt::Key_Return,    false }, { PAD_A,     Qt::Key_Backspace, false },
        { PAD_START, Qt::Key_Escape,    false },
    };
```

with

```cpp
    // ONE table, indexed identically on every surface, because padPrev_/padNext_ are indexed by row. A row
    // whose key is 0 for the current surface is inert there but still edge-tracked, so a button held while
    // the surface changes cannot fire a press it never earned.
    //
    // North is the info/mark button on both surfaces and West is the secondary action on both, so the two
    // sets do not compete for muscle memory. Every row reads through Gamepad::binding(), which means all of
    // them are remappable per port through the input panel that already exists.
    struct Nav { int id; int keyBrowse; int keyPlayer; bool repeat; };
    static const Nav navs[] = {
        { PAD_UP,     Qt::Key_Up,        Qt::Key_Up,        true  },
        { PAD_DOWN,   Qt::Key_Down,      Qt::Key_Down,      true  },
        { PAD_LEFT,   Qt::Key_Left,      Qt::Key_Left,      true  },
        { PAD_RIGHT,  Qt::Key_Right,     Qt::Key_Right,     true  },
        { PAD_B,      Qt::Key_Return,    Qt::Key_Return,    false },
        { PAD_A,      Qt::Key_Backspace, Qt::Key_Backspace, false },
        { PAD_START,  Qt::Key_Escape,    Qt::Key_Escape,    false },  // special-cased below
        { PAD_X,      Qt::Key_I,         Qt::Key_I,         false },  // Details / mark a segment
        { PAD_Y,      Qt::Key_Slash,     Qt::Key_S,         false },  // Search / skip the offered segment
        { PAD_L,      Qt::Key_F,         0,                 false },  // Filter
        { PAD_R,      Qt::Key_P,         0,                 false },  // Add to playlist
        { PAD_SELECT, Qt::Key_T,         0,                 false },  // Cycle theme
    };
    const bool onPlayer = (stack_->currentWidget() == playerPage_);
```

- [ ] **Step 4: Route the per-surface key and report the input**

Still in `pollMenuPad`, in the per-row loop, replace the generic send branch

```cpp
        if (held)
        {
            if (!padPrev_[i]) { sendNavKey(navs[i].key); padNext_[i] = padTick_ + 420; }        // press edge
            else if (navs[i].repeat && padTick_ >= padNext_[i]) { sendNavKey(navs[i].key); padNext_[i] = padTick_ + 160; } // hold-repeat
```

with

```cpp
        const int key = onPlayer ? navs[i].keyPlayer : navs[i].keyBrowse;
        if (held && key != 0)
        {
            if (!padPrev_[i]) { sendNavKey(key); padNext_[i] = padTick_ + 420; }        // press edge
            else if (navs[i].repeat && padTick_ >= padNext_[i]) { sendNavKey(key); padNext_[i] = padTick_ + 160; } // hold-repeat
```

and leave the trailing comment and `padPrev_[i] = held;` exactly as they are — a row inert on this surface must still record its held state.

In the same loop, immediately **before** the `if (navs[i].id == PAD_START)` block, add the mode report so every button (Start included) counts as controller use:

```cpp
        // Any press edge means a controller is driving: the cursor goes away and every help chip re-spells
        // itself as buttons. Silent when we are already in pad mode (see InputMode's header on signal
        // economy) — this runs on the poll timer.
        if (held && !padPrev_[i]) InputMode::instance().notePad(0);
```

Also, in the same function, right after `Gamepad* pad = retro_ ? retro_->gamepad() : nullptr;`, add:

```cpp
    InputMode::instance().setPad(pad);   // borrowed; re-set each tick so a hot-plugged pad's brand is current
```

- [ ] **Step 5: Watch for a real pointer, and own the cursor**

In `native/src/ui/MainWindow.cpp`, add near the top of the file, in the existing anonymous namespace (or a new one just below the includes):

```cpp
namespace {
// A real mouse movement is what takes the app back out of controller mode. Two conditions, both load-bearing:
// the event must be SPONTANEOUS (a uitest-injected or otherwise synthesised move must not un-hide the cursor
// mid-navigation), and the cursor must actually have MOVED (Qt re-delivers moves as widgets appear and
// disappear under a stationary pointer, which a themed slide animation does constantly).
class PointerWatch : public QObject
{
public:
    using QObject::QObject;
protected:
    bool eventFilter(QObject* o, QEvent* e) override
    {
        const QEvent::Type t = e->type();
        if (t == QEvent::MouseMove || t == QEvent::MouseButtonPress)
        {
            if (e->spontaneous())
            {
                const QPoint p = static_cast<QMouseEvent*>(e)->globalPosition().toPoint();
                if (t == QEvent::MouseButtonPress || p != last_)
                {
                    last_ = p;
                    InputMode::instance().notePointer();
                }
            }
        }
        return QObject::eventFilter(o, e);
    }
private:
    QPoint last_{ -1, -1 };
};
} // namespace
```

In the `MainWindow` constructor, after the themed views are built (anywhere before `show()`), add:

```cpp
    // Controller-aware UI: a real pointer movement leaves pad mode, and the cursor follows the mode. The
    // watch is application-wide because mouse moves are delivered to whichever child widget is under the
    // pointer, not to the window. Its filter is one enum compare for every other event type.
    qApp->installEventFilter(new PointerWatch(this));
    connect(&InputMode::instance(), &InputMode::changed, this, [this] {
        const bool wantHidden = InputMode::instance().padMode();
        if (wantHidden == padCursorHidden_) return;
        // An override cursor outranks every per-widget setCursor in the app, so the player's own idle
        // hide/show keeps running underneath without effect while a pad is driving — which is what we want.
        if (wantHidden) QApplication::setOverrideCursor(Qt::BlankCursor);
        else            QApplication::restoreOverrideCursor();
        padCursorHidden_ = wantHidden;
    });
```

Add `#include "../input/InputMode.h"` and, if not already present, `#include <QMouseEvent>` to `MainWindow.cpp`.

- [ ] **Step 6: Build the app**

```bash
cmake --build build --config Release --target everythingbox -- /MP
```

Expected: build succeeds with no new warnings.

- [ ] **Step 7: Rebuild every probe and grep the log for errors**

```bash
cmake --build build --config Release -- /MP 2>&1 | tee build/rebuild-task4.log
grep -iE "error|LNK[0-9]|C[0-9]{4}:" build/rebuild-task4.log | head
```

Expected: the grep prints nothing. (A merge or a header change that breaks a probe target has slipped past a green suite before — this is the rebuild-all-and-grep check.)

- [ ] **Step 8: Commit**

```bash
git add native/src/ui/MainWindow.h native/src/ui/MainWindow.cpp
git commit -m "feat: give the browse verbs real pad buttons and hide the cursor on a controller"
```

---

### Task 5: Mode-aware copy in the player, the settings hints and the OSK

**Files:**
- Modify: `native/src/ui/MainWindow.cpp` (three strings)
- Modify: `native/src/ui/nav/Osk.cpp` (the footer hint)

**Interfaces:**
- Consumes: `InputMode::instance().chipFor()`, `InputMode::padMode()`, `InputMode::changed` (Task 2).
- Produces: no new API.

**Locate the strings by content, not line number** — `MainWindow.cpp` is edited by other sessions and line numbers drift:

```bash
grep -n "While a video is playing" native/src/ui/MainWindow.cpp
grep -n "Press I again" native/src/ui/MainWindow.cpp
grep -n "B: delete" native/src/ui/nav/Osk.cpp
```

**This task has no probe.** These are `tr()` strings on live surfaces; the suite cannot render them. Verification is the live `EB_UITEST` pass in Task 6.

- [ ] **Step 1: Parameterise the themed settings hint**

The first `grep` hit is inside the themed settings builder, an `info(...)` row. Replace

```cpp
             tr("While a video is playing: S skips the offered segment, I marks where one starts and ends."),
```

with

```cpp
             tr("While a video is playing: %1 skips the offered segment, %2 marks where one starts and ends.")
                 .arg(InputMode::instance().chipFor(QStringLiteral("S")),
                      InputMode::instance().chipFor(QStringLiteral("I"))),
```

Guard the substitution on the mode so a mouse user still reads keys — wrap both `.arg` values:

```cpp
             tr("While a video is playing: %1 skips the offered segment, %2 marks where one starts and ends.")
                 .arg(InputMode::instance().padMode() ? InputMode::instance().chipFor(QStringLiteral("S"))
                                                      : QStringLiteral("S"),
                      InputMode::instance().padMode() ? InputMode::instance().chipFor(QStringLiteral("I"))
                                                      : QStringLiteral("I")),
```

The settings panel is rebuilt every time it opens, so it always reflects the current mode without a subscription.

- [ ] **Step 2: Parameterise the classic settings hint**

The second `grep` hit is a `QLabel` in the QWidget settings builder (the twin the two-settings-builders rule requires). Replace

```cpp
        auto* skipHint = new QLabel(tr("While a video is playing: S skips the offered segment, "
                                       "I marks where one starts and ends."));
```

with

```cpp
        auto* skipHint = new QLabel(
            tr("While a video is playing: %1 skips the offered segment, %2 marks where one starts and ends.")
                .arg(InputMode::instance().padMode() ? InputMode::instance().chipFor(QStringLiteral("S"))
                                                     : QStringLiteral("S"),
                     InputMode::instance().padMode() ? InputMode::instance().chipFor(QStringLiteral("I"))
                                                     : QStringLiteral("I")));
```

- [ ] **Step 3: Parameterise the player notice**

Replace

```cpp
            notifier_->playerNotice(tr("Intro starts here. Press I again at the end."), 4000);
```

with

```cpp
            notifier_->playerNotice(
                tr("Intro starts here. Press %1 again at the end.")
                    .arg(InputMode::instance().padMode() ? InputMode::instance().chipFor(QStringLiteral("I"))
                                                         : QStringLiteral("I")),
                4000);
```

A notice is built at the moment it is shown, so it needs no subscription either.

- [ ] **Step 4: Make the OSK footer mode-aware**

In `native/src/ui/nav/Osk.cpp`, replace

```cpp
    auto* hint = new QLabel(QStringLiteral("B: delete   Start: done   (a real keyboard types directly)"), panel());
    hint->setStyleSheet(QStringLiteral("color: #9aa0ad; font-size: 11px;"));
    hint->setWordWrap(true);
    v->addWidget(hint);
```

with

```cpp
    // The footer names whichever device is driving. It was controller-worded unconditionally, which read as
    // nonsense to a mouse user typing into it. Rebuilt on InputMode::changed() so picking up a pad while the
    // keyboard is open re-words it under the user's hands.
    auto* hint = new QLabel(panel());
    hint->setStyleSheet(QStringLiteral("color: #9aa0ad; font-size: 11px;"));
    hint->setWordWrap(true);
    auto relabelHint = [hint] {
        InputMode& im = InputMode::instance();
        hint->setText(im.padMode()
            ? QStringLiteral("%1: delete   %2: done   (a real keyboard types directly)")
                  .arg(im.chipFor(QStringLiteral("Esc")), im.chipFor(QStringLiteral("Enter")))
            : QStringLiteral("Backspace: delete   Enter: done"));
    };
    relabelHint();
    connect(&InputMode::instance(), &InputMode::changed, hint, relabelHint);
    v->addWidget(hint);
```

Add `#include "../../input/InputMode.h"` to `Osk.cpp`.

Note the pad wording uses the **Back** and **Confirm** verbs (`Esc` / `Enter`), not the literal letters — so a remapped Back renders the button the user actually mapped, and a PlayStation pad reads `○: delete   ✕: done`.

- [ ] **Step 5: Build**

```bash
cmake --build build --config Release --target everythingbox -- /MP
```

Expected: build succeeds, no new warnings.

- [ ] **Step 6: Commit**

```bash
git add native/src/ui/MainWindow.cpp native/src/ui/nav/Osk.cpp
git commit -m "feat: name the driving device in the player, settings and on-screen keyboard copy"
```

---

### Task 6: The bundled-theme chip gate, the full suite, and the live pass

**Files:**
- Create: `native/tools/check-help-chips.py`
- Modify: `native/tools/run-headless-probes.sh`

**Interfaces:**
- Consumes: `padglyphs::verbForHint`'s hint set (Task 1), read as data by the script.
- Produces: a source-level gate line in the suite.

**Why:** the suite already gates bundled-theme drift, the nav kit, and the settings builders for the same reason — a bundled theme that ships a chip the translator does not know renders a keyboard key to a controller user with nothing failing anywhere. Registry themes stay on the documented pass-through and are deliberately not gated.

- [ ] **Step 1: Write the gate script**

Create `native/tools/check-help-chips.py`:

```python
#!/usr/bin/env python3
"""Every helpsystem chip in a BUNDLED theme must be one the app can translate to a controller button.

A theme authors its help bar as keyboard chips. PadGlyphs::verbForHint owns the set the app can translate;
anything outside it is handed back untranslated, which is right for a third-party theme (it is the author's
own text) and wrong for one we ship (a controller user reads a key they cannot press). This gate holds the
bundled themes and the built-in fallback bar to the translatable set.

Exits 0 and prints nothing on success. On failure prints one line per offending chip and exits 1.
"""
import json
import pathlib
import re
import sys

HERE = pathlib.Path(__file__).resolve().parent
NATIVE = HERE.parent

# The hints PadGlyphs::verbForHint maps, read straight out of the source so the two cannot drift.
PADGLYPHS = (NATIVE / "src" / "input" / "PadGlyphs.cpp").read_text(encoding="utf-8")
KNOWN = set(re.findall(r'hintKey == QLatin1String\("([^"]*)"\)', PADGLYPHS))

# Arrow chips are a D-pad direction already and pass through by design.
ARROWS = set("\u2190\u2191\u2192\u2193")


def chip_ok(button: str) -> bool:
    if button in KNOWN:
        return True
    return bool(button) and all(ch in ARROWS for ch in button)


def walk(node, out):
    """Collect every helpsystem entry's `button` from an arbitrarily nested theme document."""
    if isinstance(node, dict):
        if node.get("type") == "helpsystem":
            for e in node.get("entries") or []:
                if isinstance(e, dict) and "button" in e:
                    out.append(str(e["button"]))
        for v in node.values():
            walk(v, out)
    elif isinstance(node, list):
        for v in node:
            walk(v, out)


def main() -> int:
    bad = []

    for theme in sorted((NATIVE / "themes2").glob("*/theme.json")):
        try:
            doc = json.loads(theme.read_text(encoding="utf-8"))
        except Exception as exc:                       # a parse break is the drift gate's business, not ours
            print(f"{theme}: could not parse ({exc})")
            return 1
        chips = []
        walk(doc, chips)
        for c in chips:
            if not chip_ok(c):
                bad.append(f"{theme.relative_to(NATIVE)}: chip {c!r} has no controller equivalent")

    # The built-in fallback bar in Theme.js, which renders for a theme that declares no view of its own.
    themejs = (NATIVE / "src" / "theme2" / "qml" / "Theme.js").read_text(encoding="utf-8")
    for c in re.findall(r'\{\s*"button"\s*:\s*"((?:[^"\\]|\\.)*)"', themejs):
        c = c.encode("utf-8").decode("unicode_escape")
        if not chip_ok(c):
            bad.append(f"src/theme2/qml/Theme.js: fallback chip {c!r} has no controller equivalent")

    for line in bad:
        print(line)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 2: Run it against the tree as it stands**

```bash
python native/tools/check-help-chips.py; echo "exit=$?"
```

Expected: `exit=0` with no output. If it prints a chip, that chip is a real gap — add its hint to `verbForHint` **and** to `probe_padglyph`'s section 2, or change the bundled theme. Do not weaken the gate.

- [ ] **Step 3: Add the gate to the suite**

In `native/tools/run-headless-probes.sh` (**CRLF file — byte-exact edit**), add immediately after the `bundled-theme / registry drift` gate's trailing blank `echo`:

```bash
# Every helpsystem chip a BUNDLED theme ships must be one PadGlyphs can turn into a controller button
# (controller-aware UI). An unknown chip is handed back untranslated — correct for a registry theme, whose
# text is its author's, and wrong for one we ship: a controller user reads a key they have no way to press,
# and nothing anywhere goes red. Registry themes are deliberately NOT gated.
echo "=== bundled help-bar chips ==="
CHIPS_PY="$HERE/check-help-chips.py"
if [ ! -f "$CHIPS_PY" ]; then
  echo "FAIL: bundled help-bar chips (check-help-chips.py not found at $CHIPS_PY)"; fail=1
elif "$PY" "$CHIPS_PY"; then
  echo "PASS: bundled help-bar chips"
else
  echo "FAIL: bundled help-bar chips — a bundled theme (or the built-in fallback bar) names a key that has"
  echo "  no controller equivalent, so a player on a pad reads a button they cannot press. Either give the"
  echo "  hint a verb in PadGlyphs::verbForHint (and cover it in probe_padglyph) or change the theme."
  fail=1
fi
echo
```

- [ ] **Step 4: Verify the runner still parses**

```bash
bash -n native/tools/run-headless-probes.sh
```

Expected: no output, exit 0. A merged-in gate section has eaten a neighbouring gate's closing `fi` before — this check is not optional.

- [ ] **Step 5: Run the whole suite**

```bash
cmake --build build --config Release -- /MP
BUILD_DIR=build bash native/tools/run-headless-probes.sh | tail -3
```

Expected: the tail ends with `ALL HEADLESS PROBES PASSED`. Then confirm the verdict file and that both new probes actually ran:

```bash
cat build/headless-probes.verdict
grep -E "PADGLYPH-OK|INPUTMODE-OK|bundled help-bar chips" build/headless-probes.log
```

Expected: `VERDICT=PASS`, and all three greps hit. A probe that is registered but never runs is the exact failure the three-place rule exists for — this grep is how you know it ran.

- [ ] **Step 6: Deploy and drive the real app**

Deploy the Release build to `C:\EverythingBox-app` (Release, not Debug — the debug DLLs are not there), then launch with the UI-test channel on and drive it through the harness:

```bash
EB_UITEST=1 "C:/EverythingBox-app/EverythingBox.exe"
```

With `native/tools/uitest.py`, confirm on the **themed home**:

1. Screenshot with the mouse having been moved: the cursor is visible and the help bar reads `Enter / Esc / I / …`.
2. Press a controller button (or inject the pad path), screenshot: the cursor is gone and the chips read the brand's buttons.
3. Move the mouse, screenshot: cursor back, chips back to keys.

Then repeat 1–3 with a **video playing** (the player's notice after marking an intro should name the button), and with the **OSK open** (its footer should re-word).

Record each screenshot; a claim that this works is not supportable without them.

- [ ] **Step 7: Commit**

```bash
git add native/tools/check-help-chips.py native/tools/run-headless-probes.sh
git commit -m "test: gate bundled help-bar chips on having a controller equivalent"
```

---

## Self-Review

**Spec coverage.** Every section of the spec maps to a task: the input authority and both mode transitions → Task 2 + Task 4 Step 5; brand detection → Task 2 Step 4; the hint translation chain and the pass-through rule → Task 1; `HelpSystem.qml` and the four hosts → Task 3; the five new browse bindings and the player's two → Task 4; the cursor → Task 4 Step 5; the three player/settings strings and the OSK footer → Task 5; `probe_padglyph`, the three-place registration and the bundled-theme gate → Tasks 1 and 6; the live `EB_UITEST` pass → Task 6 Step 6.

**Two deliberate deviations from the spec**, both recorded here so a reviewer is not surprised:

1. The spec puts `setOverrideCursor` inside the mode transition. The plan keeps `InputMode` QtCore-only and has `MainWindow` react to `changed()`, so the authority is probe-able exactly like `FormFactor`. Same behaviour, cleaner ownership.
2. The spec's Select row spelled the label `⧉ / Create / −`. The plan uses `View / Create / −` — a readable word beats an ambiguous glyph in a chip, and the PlayStation and Switch cells are unchanged.

Both should be folded back into the spec so the two documents agree.

**Placeholders.** One intentional: Task 3 Step 1 says `§N` because `probe_navqml`'s section numbering must be read from the file at implementation time, and it says exactly how to read it. Its `CHECKX` is likewise flagged as standing in for that file's own macro, with the command to find it. Nothing else is deferred.

**Type consistency.** `chipFor` is the name in `InputMode.h`, in `HelpSystem.qml`, in `Osk.cpp`, in the three `MainWindow` strings, and in `probe_inputmode` — one name throughout. `padglyphs::chip` (three args) is distinct from `InputMode::chipFor` (one arg) and is only ever called by `InputMode`. `brand()` returns `std::string` on `Gamepad` and `QString` on `InputMode`, converted once in `InputMode::brand()`. `Verb::Search` and `Verb::Skip` both return RetroPad id 1, asserted in `probe_padglyph` section 4 and relied on by the `PAD_Y` row in Task 4.
