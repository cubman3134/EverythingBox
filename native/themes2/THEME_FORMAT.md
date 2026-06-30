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
| `grid` | grid of item cards | `columns`, `aspect`, `spacing`, `card.radius`, `card.selectedBorder`, `card.selectedWidth` |
| `carousel` | horizontal strip, selected centred + enlarged | `itemWidth`, `spacing`, `color` (selection), `card.radius` |
| `rating` | five stars from a 0..1 value | `binding` (or `value`), `color`, `emptyColor` |
| `video` | preview area: a slow Ken Burns drift over the bound poster + a play badge | `path`/`binding`, `radius` |
| `helpsystem` | row of button hints | `entries: [{button,label}, …]`, `color`, `fontSize` |
| `particles` | animated background field | `preset`, `count`, `color`, `dotSize`, `speed`, `image` |

`grid` and `carousel` render the home's catalog rows (each `{title, accent, image}`) and follow the selection. Exactly one of them is usually the main element; place a `text`/`image`/`rating` bound to `selected.*` nearby to show details for the focused item.

### Background images & particles

Every view already supports a **background image**: `"background": { "image": "bg.jpg", "dim": 0.4 }` (path relative to the theme folder; `dim` is a 0..1 black overlay for readability). For a moving picture, place a full-screen `image` element (`pos: [0,0]`, `size: [1,1]`) at a low `zIndex` instead.

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

Copy one of the shipped themes (`Default`, `Grid`, `Lumen`, `Midnight`) as a starting point and edit away — the home updates as you save.
