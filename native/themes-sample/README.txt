My Media Vault — Themes
=======================

Drop a "<Name>.json" file into this folder, then pick it under Settings -> Theme.
Each theme is per profile. Assets (background images, icons) are relative to this folder.

COLOURS
  name              Display name in the picker (defaults to the file name).
  accentFollowsTab  true  = chrome/background follow the selected media tab's colour (default).
                    false = use one fixed "accent" colour everywhere.
  accent            Fixed accent colour (used when accentFollowsTab is false), e.g. "#C0392B".
  neutralTab        Colour of the un-selected media tabs, e.g. "#9A8480".
  tabColors         Per-media-type colour overrides:
                      { "movie": "#C0392B", "music": "#7D3C98", "podcast": "#1ABC9C" }

LOOK & FEEL (ES-style)
  fontFamily        UI font family, e.g. "Georgia" ("" = system default).
  fontScale         Scales the whole UI font, e.g. 1.1.
  cornerRadius      Rounding of the search box, e.g. 14.
  background        A background image filename in this folder, e.g. "aurora-bg.png".
  backgroundDim     0..1 overlay opacity over the background (higher = lighter, more readable). Default 0.55.
  icons             Per-type placeholder icon images (svg/png) in this folder:
                      { "podcast": "podcast.svg", "movie": "film.png" }

DETAIL PAGE LAYOUT (a simple declarative "view")
  detail.image      "left" (cover beside the text, default) | "top" (cover above) | "hidden".
  detail.imageWidth Cover width in px (height keeps poster ratio), e.g. 200.
  detail.order      Order of the text rows, any of:
                      ["title", "favorite", "facts", "overview"]
                    Omitted rows are appended (nothing disappears).

Built-in themes: Default, Sunset, Ocean, Grape, Slate.
Example themes shipped here: Crimson (colours), Aurora (background + font + detail layout).
