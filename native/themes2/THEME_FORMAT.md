# My Media Vault — theme format

A theme is a folder under the app's `themes2/` directory containing a `theme.json`:

```
<app>/themes2/
  Default/theme.json
  Midnight/theme.json
  MyTheme/theme.json      <- your theme
```

Pick it (and turn the themed home on) in **Appearance** — press **Ctrl+Shift+A**. The dialog shows a **live preview** as you select. Editing a `theme.json` updates the home **live** (hot-reload) while the themed home is on.

The whole layout is **resolution-independent**: positions, sizes and font sizes are **fractions** of the screen, so a theme looks the same at any window size (and scales down in the preview).

## File shape

```json
{
  "name": "My Theme",
  "author": "you",
  "views": {
    "home": {
      "background": { "color": "#101216", "image": "bg.jpg", "dim": 0.4 },
      "elements": [ /* … */ ]
    }
  }
}
```

- `name`, `author` — shown in the theme picker.
- `views` — one or more named views, each with the same shape (`background` + `elements`) binding to the
  same data:
  - `home` — the main screen (the media-type catalogs as a carousel/grid). **/** opens the highlighted
    catalog and searches within it.
  - `browse` — the "gamelist" shown when you open a catalog: a `grid` of that catalog's items plus a detail
    panel bound to `selected.*` (`title`, `subtitle`, `image`). Navigate to focus, **Enter** to open/drill,
    **Esc** to go up, **/** to search within the catalog (large catalogs page in as you scroll near the end).
  - `detail` — shown for the focused item when you press **I** (Info); **Esc** returns. Typically a big
    `selected.image`, `selected.title`, `selected.rating`, `selected.overview`.
- `background.color` — hex. `background.image` — a path **relative to the theme folder** (optional). `background.dim` — 0..1 black overlay over the image, for readability.

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

- `selected.title`, `selected.subtitle`, `selected.image`, `selected.rating` — the currently-focused row.
- `system.name`, `index`, `count`.

Example: `{ "type": "text", "binding": "selected.title" }`.

## Colours & fonts

- Colours are hex strings, e.g. `"#E07A2E"`.
- `fontSize` is a **fraction of screen height** (e.g. `0.04` ≈ 4% tall). `fontFamily` optional. `bold` true/false. `align`: `left`|`center`|`right`.

## Elements

| `type` | Purpose | Key properties |
| --- | --- | --- |
| `text` | literal or bound text | `text` or `binding`, `color`, `fontSize`, `align`, `bold`, `wrap`, `lines` |
| `datetime` | live clock/date | `format` (Qt format, e.g. `"hh:mm"`, `"ddd d MMM"`), `color`, `fontSize`, `align` |
| `image` | poster / picture | `path` or `binding`, `fillMode` (`contain`\|`cover`\|`stretch`), `radius`, `color` (placeholder) |
| `grid` | grid of item cards | `columns`, `aspect`, `spacing`, `card.radius`, `card.selectedBorder`, `card.selectedWidth`, `card.fill`, `card.border`+`card.borderWidth` (always-on outline), `card.selectedScale` (the selected card grows + lifts), `card.label` (`overlay`\|`center` name centred on the card, no bar\|`top` title bar on the card\|`below` name-plate\|`none`), `card.labelSize`, `card.labelColor`, `card.labelBg` |
| `button` | a clickable button that runs a named host action | `action` (`settings`\|`profile`\|`appearance`), `glyph` (`settings`\|`profile`), `label`, `color`, `textColor`, `borderColor`, `shape` (`pill`\|`round`) |
| `panel` | a filled bar with an optional curved (dipped) top edge — the Wii-menu bottom shelf | `color` or `gradient` `["#top","#bottom"]`, `curve` (0..1 middle dip), `topColor`+`topWidth` (accent line) |
| `channels` | a Wii-menu paged grid: fixed `columns`×`rows` pages, greyed-out empty slots pad out the pages, left/right page arrows in the gutters browse into the empty pages | `columns`, `rows`, `pages` (minimum page count — extra pages are empty slots to arrow into), `spacing`, `card.*` (as `grid`, label centred), `card.emptyFill`, `card.emptyBorder` |
| `carousel` | horizontal strip, selected centred + enlarged | `itemWidth`, `spacing`, `color` (selection), `card.radius` |
| `rating` | five stars from a 0..1 value | `binding` (or `value`), `color`, `emptyColor` |
| `video` | preview area: a slow Ken Burns drift over the bound poster + a play badge | `path`/`binding`, `radius` |
| `helpsystem` | row of button hints | `entries: [{button,label}, …]`, `color`, `fontSize` |
| `particles` | animated background field | `preset`, `count`, `color`, `dotSize`, `speed`, `image` |
| `xmb` | PlayStation-style cross (categories × items) | `color`, `subColor`, `descColor`, `crossX`, `crossY`, `catSpacing`, `itemSpacing`, `iconSize` |
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

A theme whose **`home`** view contains an `xmb` element becomes a two-axis cross instead of a carousel/grid: the horizontal axis is your media-type categories, and the vertical axis is the highlighted category's **live** items (games under Game, music under Music, …). **←→** switch category (the column reloads), **↑↓** move through the column, **Enter** opens/drills, **Esc** goes up, **/** searches the current category. The last category on the cross is a synthetic **Settings** (opens Appearance). The `xmb` element draws categories as accent tiles (first letter as a stand-in) — drop an `icon` (relative image path) on a category for real art. An xmb home needs no `browse`/`detail` view; the cross is the whole screen. Pair it with a `wave` and a `datetime` for the full look (see the shipped **XMB** theme).

Note: the front end is software-rendered (so it coexists with the video engine). Stacking several heavy animated elements (e.g. a high-`segments` `wave` **and** `particles` **and** the `xmb` cross) can exceed the renderer's budget — keep `wave.segments` modest and avoid piling animated fields on an xmb home.

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

Any action you leave out is silent. Files must be uncompressed **WAV** (PCM) — that's what the low-latency player supports; keep them short (a few-frame click/blip). The shipped **Midnight** theme has a `sounds/` folder you can copy.

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

Copy one of the shipped themes (`Default`, `Grid`, `Lumen`, `Midnight`, `Channels`) as a starting point and edit away — the home updates as you save.
