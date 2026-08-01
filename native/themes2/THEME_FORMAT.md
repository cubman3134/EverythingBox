# EverythingBox — theme format

A theme is a folder under the app's `themes2/` directory containing a `theme.json`:

```
<app>/themes2/
  Channels/theme.json
  Triple/theme.json
  MyTheme/theme.json      <- your theme
```

Pick it (and turn the themed home on) in **Appearance** — press **Ctrl+Shift+A**. The dialog shows a **live preview** as you select. Editing a `theme.json` updates the home **live** (hot-reload) while the themed home is on.

The whole layout is **resolution-independent**: positions, sizes and font sizes are **fractions** of the screen, so a theme looks the same at any window size (and scales down in the preview).

## File shape

```json
{
  "name": "My Theme",
  "author": "you",
  "formFactors": ["desktop", "tv", "mobile", "handheld"],
  "views": {
    "home": {
      "background": { "color": "#101216", "image": "bg.jpg", "dim": 0.4 },
      "elements": [ /* … */ ]
    }
  }
}
```

- `name`, `author` — shown in the theme picker.
- `formFactors` — optional; the device types your layout is built for. See **Form factors** below.
- `views` — one or more named views, each with the same shape (`background` + `elements`) binding to the
  same data:
  - `home` — the main screen (the media-type catalogs as a carousel/grid). **/** opens the highlighted
    catalog and searches within it.
  - `browse` — the "gamelist" shown when you open a catalog: a `grid` of that catalog's items plus a details
    pane bound to `selectedMeta.*` (the live metadata for the hovered row — see **Data bindings**). Navigate
    to focus, **Enter** to open/drill, **Esc** to go up, **/** to search within the catalog (large catalogs
    page in as you scroll near the end).
  - `detail` — shown for the focused item when you press **I** (Info); **Esc** returns. Typically a big
    `selected.image`, `selected.title`, `selected.rating`, `selected.overview`.
  - `nowplayingAudio` — optional: the audiobook/music now-playing page (audio has no picture, so this screen
    replaces the classic player page). Give it one full-screen `nowplayingaudio` element; a theme that omits
    the view keeps the classic player page instead.
- `background.color` — hex. `background.image` — a path **relative to the theme folder** (optional). `background.dim` — 0..1 black overlay over the image, for readability.

**Undeclared views fall back, they do not go blank.** If the app navigates to a view your theme does not
declare (or declares with no `elements`), the engine renders a plain built-in layout for it — the view's
title, a grid of its items and the help bar — over **your** `home` background, with ink picked light or
dark to stay readable on it, and outlined so it stays readable over a background `image` too. It is a
safety net, not a design: a theme that styles the view always wins. It exists because a missing view used
to paint the background and nothing else, which is a screen the user can navigate and select on but cannot
see (issue #29).

A view declared with an **empty** `elements` list counts as not declared **everywhere**, not just in the
renderer: `"detail": { "elements": [] }` does not switch **I** (Info) on, and an empty `nowplayingAudio`
keeps the classic player page. Declaring the key is not enough — give the view elements or leave it out.

## Form factors — which devices your theme is for

The app runs on desktops, on TVs across the room and on phones, and it tells your theme which it is: the
`form.*` values (`form.mode`, `form.uiScale`, `form.minHitPx`, `form.safeAreaFrac`, `form.density`) are live
in every view. But scaling is not the same as *fitting* — a three-pane desktop layout with 12-pixel captions
scales up on a TV and is still unreadable from a sofa. So a theme may **declare** where it is meant to work:

```json
"formFactors": ["desktop", "tv", "mobile", "handheld"]
```

| label | means |
| --- | --- |
| `desktop` | a mouse, a keyboard and a window |
| `tv` | a couch, a remote or a pad, and text that has to read from three metres |
| `mobile` | a phone: touch targets, one column, a notch to stay clear of |
| `handheld` | a Steam-Deck-ish device: a small screen at arm's length with physical controls |

**What it does.** The theme picker (and the theme list in **Appearance**) puts a short note under any theme
that does not list the device you are on. That is *all* it does. Your theme is never hidden, never moved down
the list and never made unselectable — a user who deliberately installed your theme and then could not find
it would reasonably conclude the app was broken, so the app tells them and lets them decide.

**Leaving it out is fine.** A theme with no `formFactors` is *undeclared*, which the app treats as **unknown
— not as "works everywhere" and not as "works nowhere"**. Its row gets a quieter note saying support was not
declared. Every theme written before this key existed is in exactly that state and every one of them still
works; the note is the app admitting it has nothing to go on, not a complaint about your theme. Declare the
key when you know the answer, and leave it out when you don't — an honest "unknown" beats a guessed "yes".

**It is a claim, not a measurement.** Nothing checks it. The app cannot tell a correct declaration from a
wishful one, so `["desktop", "tv", "mobile", "handheld"]` on a theme that only really works on a desktop is
worse than no declaration at all: it converts "we don't know" into a confident wrong answer. List the devices
you have actually looked at your theme on.

**Judged against the device in front of the user, every time.** `Display mode` is per-device and its `Auto`
setting re-resolves on each one, so the same profile synced to a phone and to a TV can have a theme that
suits one and not the other. The note follows the device, not the profile.

**`handheld` is advisory today.** The app resolves exactly three modes — `desktop`, `tv` and `mobile` — so it
cannot check a handheld claim, and it does not pretend to: the label is recorded and shown to people, and it
matches nothing. A theme declaring **only** `["handheld"]` therefore reads as "not listed for this device"
everywhere, which is the honest reading — you did not claim the device the user is on. If a real handheld
mode is ever added, themes that already list it start matching it with no change needed. List `handheld`
alongside the modes you actually support, not instead of them.

Two shapes that do not do what they look like:

- **`"formFactors": []`** is a real declaration meaning *fits nothing*, so it is noted on every device. If
  you meant "I haven't decided", omit the key.
- **`"formFactors": "desktop"`** — a bare string, not an array — is read as **undeclared**. The app will not
  guess at a malformed declaration, because guessing wrong is exactly the confident-wrong-answer problem
  above. The "support not declared" note on a theme you thought you had declared is how you find the typo.

Entries are matched case- and space-insensitively, so `"TV"` and `" desktop "` are fine.

Both shipped themes (`Channels`, `Triple`) declare all four. `Night` declares only `["desktop"]` — it is a
dense sidebar-and-details-pane layout built for a mouse, and it says so.

## Positioning (every element)

| Key | Meaning |
| --- | --- |
| `pos`    | `[x, y]` — anchor point, as fractions of the screen (0..1) |
| `size`   | `[w, h]` — element size, as fractions of the screen |
| `origin` | `[ox, oy]` — which point of the element sits at `pos` (`[0,0]` = top-left, `[0.5,0.5]` = centre, `[1,1]` = bottom-right) |
| `zIndex` | stacking order (higher = on top) |
| `opacity`| 0..1 |
| `id`     | optional name (for your own reference) |

So an element's screen rectangle is: `x = pos.x*W − origin.x*(size.w*W)`, likewise for y.

## Data bindings

Text/image/video/rating elements can show **live data** instead of a literal, via `"binding"` — a path into the data context:

- `selected.*` — the currently-focused row: `title`, `subtitle`, `image`, `rating`. In the **detail** view
  `selected` is that item's *full* metadata instead, so the same paths work there and carry more:
  `overview`, `factsText`, and the artwork roles (`logo`, `box`, `hero`, `images.screenshot`, …).
- `selectedMeta.*` — the live metadata for the row you are hovering (see below).
- `system.name`, `index`, `count`.

Example: `{ "type": "text", "binding": "selected.title" }`.

### `selectedMeta.*` — the live hover metadata

`selected.*` is the row itself (in the `detail` view, the full detail record). **`selectedMeta.*`** is the
richer metadata fetched *for the row you are hovering*: a skeleton appears the instant the selection moves
(`title`, `subtitle`, `image`, `type`, `accent`, `favorite`) and is enriched a moment later with `overview`,
`factsText`, art roles (`logo`, `box`, `hero`, `screenshot`, … plus the full `images` / `videos` / `audio`
lists) and `lastPlayed` / `timePlayed` where they exist. This is what a **details pane beside a grid** binds:

```json
{ "type": "text", "binding": "selectedMeta.overview", "wrap": true, "lines": 16 }
```

**Only some surfaces drive it.** The fetch is keyed by a *browse index*, so it runs on the grid **`browse`**
view and on an **`xmb` `home` while drilled into a catalog** — the surfaces whose rows are catalog items. The
grid **`home`** is deliberately not one: its rows are catalogs, not items, and have no browse index to fetch.
A pane bound to `selectedMeta` on a grid home therefore renders **empty**, silently — bind `selected.*` there
instead. The shipped **Night** theme does exactly that: `selected.*` on its home pane, `selectedMeta.*` on its
browse pane.

### Facts, and other structured data

An item's labelled facts — Developer, Genre, Players, Rating, … — are published as a ready-to-draw string
under **`factsText`**: `"Developer: Square     •     Genre: RPG"`. It exists on both metadata shapes,
formatted identically, so the same line works on either surface:

```json
{ "type": "text", "binding": "selectedMeta.factsText", "wrap": true, "lines": 3 }
{ "type": "text", "binding": "selected.factsText",     "wrap": true, "lines": 4 }
```

A `text` element renders a **scalar**. Bindings that resolve to structured data instead — `facts` (the raw
list of label/value pairs behind `factsText`) or `images` (the artwork role map) — render as **nothing**
rather than as garbled object text. So if a facts line comes up blank, check that you bound `factsText` and
not `facts`. **Night** binds it on both its `browse` pane and its `detail` page.

## Colours & fonts

- Colours are hex strings, e.g. `"#E07A2E"`. Any form Qt reads is accepted, so `"#RGB"` (`"#EEE"`),
  `"#RRGGBB"`, `"#AARRGGBB"` and the SVG colour **names** (`"white"`, `"whitesmoke"`) all work. A string Qt
  cannot read is **not** ignored — it paints **black**, so a typo'd colour is a black box, not the default.
  There is no 3-digit-plus-alpha form: `"#FFF8"` is a typo, not 50 % white.
- `fontSize` is a **fraction of screen height** (e.g. `0.04` ≈ 4% tall). `fontFamily` optional. `bold` true/false. `align`: `left`|`center`|`right`.

## Elements

| `type` | Purpose | Key properties |
| --- | --- | --- |
| `text` | literal or bound text | `text` or `binding`, `color`, `fontSize`, `align`, `bold`, `wrap`, `lines`, `outline` (a contrasting halo colour, for text over an image) |
| `datetime` | live clock/date | `format` (Qt format, e.g. `"hh:mm"`, `"ddd d MMM"`), `color`, `fontSize`, `align`, `fontFile` (a bundled font in the theme folder, e.g. `"fonts/Foo.ttf"`) or `fontFamily` (a system font) |
| `image` | poster / picture | `path` or `binding`, `fillMode` (`contain`\|`cover`\|`stretch`), `radius`, `color` (placeholder) |
| `grid` | grid of item cards | `columns`, `aspect`, `spacing`, `card.radius`, `card.selectedBorder`, `card.selectedWidth`, `card.fill`, `card.border`+`card.borderWidth` (always-on outline), `card.selectedScale` (the selected card grows + lifts), `card.label` (`overlay`\|`center` name centred on the card, no bar\|`top` title bar on the card\|`below` name-plate\|`none`), `card.labelSize`, `card.labelColor`, `card.labelBg` |
| `button` | a clickable button that runs a named host action (`round` gets a Wii-style bevel + soft shadow; `housing` puts it on a disc that runs off the screen corner). Also keyboard/controller reachable: press Down at the bottom row of the grid to focus the buttons, Left/Right between them, Up back to the grid | `action` (`settings`\|`profile`\|`appearance`), `glyph` (`settings`\|`profile`), `label`, `color`, `textColor`, `borderColor`, `shape` (`pill`\|`round`), `housing`, `housingSide` (`left`\|`right`), `housingScale` |
| `clock` | the current time as a Digital-7-style 7-segment display (angled segments + slant) + a small AM/PM | `onColor` (lit segments), `offColor` (unlit "ghost" segments), `ampmColor`, `ampmSize` (fraction of height), `thickness` (segment thickness, fraction of a digit's width), `slant` (italic lean; 0 = upright), `align` (`center`\|`left`) |
| `panel` | a filled bar with a shaped top edge — the Wii-menu bottom shelf (flat sides, an eased dip, flat middle) | `color` or `gradient` `["#top","#bottom"]`, `curve` (0..1 middle dip), `sideFlat`+`midFlat` (flat runs at the ends / middle, as a fraction of width), `topColor`+`topWidth` (accent line) |
| `channels` | a Wii-menu paged grid: fixed `columns`×`rows` pages, greyed-out empty slots fill the last page and a sliver of the next column always peeks off the right edge; the left/right arrows page only between pages that hold channels (never onto an all-empty page) | `columns`, `rows`, `peek` (width of the peeking column, in cells — default 0.35), `spacing`, `card.*` (as `grid`, label centred), `card.emptyFill`, `card.emptyBorder` |
| `carousel` | horizontal strip, selected centred + enlarged | `itemWidth`, `spacing`, `color` (selection), `card.radius` |
| `rating` | five stars from a 0..1 value | `binding` (or `value`), `color`, `emptyColor` |
| `video` | preview area: a slow Ken Burns drift over the bound poster + a play badge | `path`/`binding`, `radius` |
| `helpsystem` | row of button hints | `entries: [{button,label}, …]`, `color`, `fontSize`, `outline` (as `text`) |
| `particles` | animated background field | `preset`, `count`, `color`, `dotSize`, `speed`, `image` |
| `xmb` | PlayStation-style cross (categories × items) | `color`, `subColor`, `descColor`, `crossX`, `crossY`, `catSpacing`, `itemSpacing`, `iconSize` |
| `sidebar` | a vertical rail of the media-type **categories** beside a grid — the non-XMB way into that zone (see below) | the rail: `fill`, `radius`, `border`+`borderWidth`, `title`, `titleColor`, `titleSize`. The rows: `rowHeight`, `rowSpacing`, `rowRadius`, `fontSize`, `fontFamily`, `bold`, `color`, `selectedColor`, `selectedBg` (selected, rail unfocused), `focusBg` (selected, rail focused — the focus ring), `accentBar`, `showIcons` |
| `gallery` | the selected item's screenshot / fanart reel, cross-fading on a timer | `role` (default `screenshot`), `fallback` (another role, or a literal path, when the item has none), `interval` (ms between images, min 800, default 4000), `fillMode` (`cover`\|`contain`), `radius`, `color` (backdrop) |
| `actionrow` | the `detail` view's row of action pills — Play/Read, Choose source, Download, Favorite, Playlist, Hide, Status, Tags, the external-player pair. The verbs are chosen per item by the host; ←→ move between them, Enter runs one | `fontSize` |
| `nowplaying` | the current background-music track (scrolls sideways when the name is wider than the box; hidden when nothing plays) | `color`, `fontSize`, `align`, `bold`, `prefix` (default `"♪  "`) |
| `nowplayingaudio` | the **whole** audio now-playing page — cover, title/author, track line, progress bar, transport strip and queue list. Place one full-screen in a `nowplayingAudio` view; everything it shows is host-fed | `accent`, `color`, `dimColor`, `panelColor`, `titleSize`, `subSize`, `metaSize` |
| `wave` | flowing translucent bands | `color`, `bands` (1-4), `amplitude`, `speed`, `segments` |

`grid` and `carousel` render the home's catalog rows (each `{title, accent, image}`) and follow the selection. Exactly one of them is usually the main element; place a `text`/`image`/`rating` bound to `selected.*` nearby to show details for the focused item.

### Background images & particles

Every view already supports a **background image**: `"background": { "image": "bg.jpg", "dim": 0.4 }` (path relative to the theme folder; `dim` is a 0..1 black overlay for readability). For a moving picture, place a full-screen `image` element (`pos: [0,0]`, `size: [1,1]`) at a low `zIndex` instead. A view can also use a vertical **gradient** instead of a flat colour: `"background": { "gradient": ["#EEF3FA", "#C6D3E7"] }` (top → bottom).

The **`particles`** element is an animated field for ambiance — usually full-screen (`pos: [0,0]`, `size: [1,1]`) behind your content (`zIndex: 0`). It is rendered with plain animated items so it works on the front end's software renderer (native `QtQuick.Particles`, which needs the GPU, would not draw here).

| key | meaning |
| --- | --- |
| `preset` | `snow` (drifting down), `rain` (fast streaks), `embers` (rising, fading), `stars` (twinkle in place), `bokeh` (big soft drift), `dust` (slow motes) |
| `count` | number of particles (capped at 400 — software-rendered, so keep it modest) |
| `color` | particle colour (hex) |
| `dotSize` | particle size as a fraction of the element height (e.g. `0.008`) |
| `speed` | speed multiplier (default `1`) |
| `image` | optional: draw this image (relative path) per particle instead of a dot |

Use the element's own `opacity` to dim the whole field. Note `dotSize`/`speed` are dedicated names because the layout keys `size` (`[w,h]`) and `opacity` are consumed by the element's frame. Example: `{ "type": "particles", "pos": [0,0], "size": [1,1], "origin": [0,0], "zIndex": 0, "opacity": 0.5, "preset": "stars", "count": 90, "color": "#ABB6E0", "dotSize": 0.006 }`.

### XMB (the PlayStation cross)

A theme whose **`home`** view contains an `xmb` element becomes a two-axis cross instead of a carousel/grid: the horizontal axis is your media-type categories, and the vertical axis is the highlighted category's **live** items (games under Game, music under Music, …). **←→** switch category (the column reloads), **↑↓** move through the column, **Enter** opens/drills, **Esc** goes up, **/** searches the current category. The last category on the cross is a synthetic **Settings** (opens Appearance). The `xmb` element draws categories as accent tiles (first letter as a stand-in) — drop an `icon` (relative image path) on a category for real art. Pair it with a `wave` and a `datetime` for the full look (see the shipped **Triple** theme).

**An xmb home still needs a `browse` view.** This file used to claim the opposite — "the cross is the whole screen" — and that claim is what caused issue #29. It is nearly true: the cross replaces the browse *grid* for catalogs, because drilling into a catalog swaps the column in place. But **/** at the cross ROOT is a *cross-add-on* search, and its merged results are not one category's column — they open the **`browse`** view like any other gamelist. A theme that declares no `browse` therefore rendered that screen as a bare background: navigable, selectable and completely blank. Declare one (Triple's is a plain grid over the same wave), or accept the engine's built-in fallback below.

Note: the front end is software-rendered (so it coexists with the video engine). Stacking several heavy animated elements (e.g. a high-`segments` `wave` **and** `particles` **and** the `xmb` cross) can exceed the renderer's budget — keep `wave.segments` modest and avoid piling animated fields on an xmb home.

### Sidebar (the category rail)

A view that contains a **`sidebar`** element renders the media-type **categories** as a vertical rail beside
its grid. That declaration is the theme's **opt-in**: the engine scans every view, and one `sidebar` anywhere
switches the whole widget's categories zone to the rail shape (exactly how an `xmb` element opts into the
cross — there is no separate flag). Declare the element in each view that should show it; the shipped
**Night** theme puts an identical rail on both `home` and `browse`.

Until the sidebar existed, `xmb` was the only element that read `categories`, so a theme wanting a category
rail beside a grid had to declare the cross — which cost 2-D grid stepping and disabled the bottom `buttons`
bar. A `sidebar` costs neither. The behaviour, which is as much the contract as the knobs are:

- **Left** at the grid's **leftmost column** crosses into the rail. Everywhere else Left/Right stay ordinary
  grid steps, so 2-D stepping is exactly what it was.
- **Up/Down** in the rail step the categories and **reload the grid live**, the same switch-as-you-move feel
  as the cross.
- **Right** (or **Enter**) returns to the grid at its **remembered** index — your 2-D position survives the
  round trip. **Esc** leaves the same way, silently.
- The bottom `buttons` bar **stays live**: Down at the grid's bottom row still reaches it, unlike on an
  `xmb` home.
- **/** (search), **F** (filter), **I** (info) and **T** (cycle theme) keep working from the rail.
- The selected row is always marked, so the current section stays readable; it takes the brighter `focusBg`
  plate only while the rail actually holds the cursor (that's the focus ring). Clicking a row switches to it.
- A long category list scrolls inside the rail instead of overflowing it.

Every knob is optional — the defaults render a plain dark rail. The fraction-valued ones are fractions of the
**view height**, like `fontSize` everywhere else: `rowHeight` (default `0.072`), `rowSpacing` (`0.004`),
`fontSize` (`0.026`), `titleSize` (`0.022`). `accentBar` (default `true`) draws a bar down the selected row's
leading edge tinted by that category's own accent, and `showIcons` (default `true`) draws each row's `icon`
image, or its accent swatch when it has none.

## Settings panel styling

`views` covers the screens a theme lays out. Everything else the app shows — every **settings** surface, the
**theme picker**, and the nav **overlays** (menus, confirms, the on-screen keyboard) — is a *panel*: the app
owns its layout, and the theme only colours it. One top-level **`settingsPanel`** block (a sibling of
`views`) does that for all of them:

```json
"settingsPanel": {
  "background":  "#0A0C10",
  "panel":       "#111419",
  "row":         "#141821",
  "rowSelected": "#1E2A3E",
  "accent":      "#4C8DFF",
  "text":        "#E9EDF4",
  "dim":         "#97A0AF",
  "separator":   "#2A313C",
  "warning":     "#E0574E"
}
```

| key | what it colours |
| --- | --- |
| `background` | a settings page behind the rows (and its title bar) |
| `panel` | an overlay's own panel (a menu, a confirm, the keyboard); a settings page's title-bar hairline derives from it |
| `row` | a resting row's card, and the Back button |
| `rowSelected` | the selected row's card / the highlighted overlay entry |
| `accent` | the selected row's border, the Back button when it holds focus, an ON toggle, a choice value + its ▾, a progress bar's fill |
| `text` | row labels and values |
| `dim` | secondary values, the `›` chevron, an empty field's `—`, a log row |
| `separator` | the spaced uppercase section headers; an overlay panel's border |
| `warning` | a destructive row's label (Reset, Delete, …) |

Every key is optional and falls back **hard** to a dark default, so a theme with no `settingsPanel` still
renders — it just won't match the theme. All three shipped themes (`Channels`, `Triple`, `Night`) declare one;
copy the block from the closest of them and retune it.

## Sounds

A theme can play a short sound when you act. Add a top-level **`sounds`** object (sibling of `views`) mapping an action to a **WAV** file relative to the theme folder:

```json
"sounds": {
  "navigate": "sounds/move.wav",
  "select":   "sounds/select.wav",
  "back":     "sounds/back.wav",
  "details":  "sounds/info.wav",
  "theme":    "sounds/swap.wav",
  "volume":   0.6
}
```

| action | plays when |
| --- | --- |
| `navigate` (alias `move`) | the selection actually moves (arrows) |
| `select` (alias `open`) | **Enter** — open / drill in |
| `back` | **Esc** / Back at a root view |
| `details` | **I** opens the detail view |
| `theme` | **T** cycles the theme |
| `volume` | 0..1 applied to all of this theme's sounds (default `0.7`) |

Any action you leave out is silent. Files must be uncompressed **WAV** (PCM) — that's what the low-latency player supports; keep them short (a few-frame click/blip). **Channels** and **Triple** both ship a `sounds/` folder you can copy.

## Where your files may live

Every path in a `theme.json` — `background.image`, an `image` element's `path` or `fallback`, a `gallery`
`fallback`, `fontFile`, a category `icon`, a `particles` `image`, `sounds`, `music` — is **relative to your
own theme folder**, and may only point at a file **inside it**. That has always been how this document
described them; it is now enforced, because a theme can install itself from the public registry and a
manifest is not a thing to be trusted with the rest of the disk.

Refused, and silently ignored (the element draws its placeholder, the sound stays silent):

| shape | example | why |
| --- | --- | --- |
| leaving your folder | `"../Channels/bg.jpg"`, `"../../../secret.png"` | another theme's files, or the user's, are not yours to read |
| an absolute path | `"C:/Users/me/art/bg.jpg"`, `"/home/me/bg.jpg"` | it only exists on *your* machine — your theme would render blank for everyone else |
| a backslash or a drive letter | `"art\\bg.jpg"`, `"C:art.png"` | refused on every platform, so a theme behaves the same everywhere |
| a **remote URL** | `"https://my-cdn.example/bg.jpg"` | see below |

**Remote URLs are not allowed for a theme's own assets.** Ship the file in your theme folder instead. A theme
that fetched its artwork from a server would contact that server every time someone's home screen drew,
would show different art after it was published than when it was reviewed, and would render blank for anyone
offline. (Artwork that comes from a *provider* — posters, logos, screenshots you reach through `binding` or
`role` — is unaffected; those are urls the addon supplies, not paths your theme names.)

## Minimal example

```json
{
  "name": "Tiny", "author": "you",
  "views": { "home": {
    "background": { "color": "#111418" },
    "elements": [
      { "type": "text", "pos": [0.04,0.06], "size": [0.6,0.07], "origin": [0,0.5],
        "text": "My Library", "color": "#FFFFFF", "fontSize": 0.045, "bold": true },
      { "type": "grid", "pos": [0.04,0.16], "size": [0.92,0.78], "origin": [0,0],
        "columns": 6, "aspect": 1.4, "card": { "radius": 12, "selectedBorder": "#E07A2E" } }
    ]
  }}
}
```

A top-level **`"hideAppearanceTile": true`** stops the app adding its Appearance catalog tile to the home grid — use it when your theme provides its own settings/appearance `button` (so it isn't offered twice).

Copy one of the shipped themes as a starting point and edit away — the home updates as you save. `Channels`
is a grid + corner buttons, `Triple` is the XMB cross, and `Night` is a `sidebar` rail + grid + a
`selectedMeta` details pane (and a `nowplayingAudio` page).

More themes — including ones that used to ship with the app — live in the community theme registry at
<https://github.com/cubman3134/everythingbox-themes>. Drop a downloaded theme folder into `themes2/` and it
shows up in **Appearance** alongside the shipped two.
