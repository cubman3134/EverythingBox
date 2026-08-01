# Theme2 asset containment — a manifest may only name files inside its own folder

Date: 2026-08-01

## The problem

`ThemeAssetPath::resolve` (added by cc39991) refuses any theme-manifest asset path that lands
outside the theme's own folder. It is reached only from `Theme.cpp`'s `resolveAsset`, which is
reached only from `parseTheme` ← `userThemes()`, which has no callers: `ThemeStore::all()` returns
`builtinThemes()` alone. The classic colour-theme system is retired, so that fix guards nothing that
runs.

The live theme2 system — the one the in-app theme gallery installs into from the public,
third-party-writable registry — resolves its own assets in three places, none of which check
containment:

1. `native/src/theme2/qml/ThemeView.qml` `function resolve(p)` — the main one, used for every
   theme-declared image. It is *more* permissive than the classic resolver was: any string
   containing `://` is returned as-is (a manifest can make the app fetch `http://attacker/x.png` on
   every render), any absolute path becomes `file:///…`, and anything else is joined as
   `base + "/" + p` — so `../OtherTheme/x.png` or `../../../secret.png` escapes.
2. `native/src/theme2/ThemeEngine.cpp` `loadEffect()` — `theme.json` `"sounds"` paths, via
   `QDir(themeDir).absoluteFilePath(file)`.
3. `native/src/ui/MainWindow.cpp` `applyThemeMusic()` — `theme.json` `"music"`, same call.

## What makes this more than a copy of cc39991

`ThemeView.qml`'s `resolve` is a single funnel carrying **two different trust domains**. Its ~15 call
sites split cleanly:

**Manifest strings** (written by whoever authored the `theme.json`):

| Site | Value |
| --- | --- |
| `ThemeView.qml` background | `view.background.image` |
| `elements/DateTime.qml` | `el.fontFile` |
| `elements/Particles.qml` | `el.image` |
| `elements/Xmb.qml` | `cat.icon` |
| `elements/Sidebar.qml` | `row.icon` |
| `elements/Image.qml`, `Video.qml` | the literal `path` / literal `fallback` branches of `T.imageSource` |
| `elements/Gallery.qml` | the literal `fallback` branch |

**Content urls** (supplied by addons, providers, scrapers and the local library):

| Site | Value |
| --- | --- |
| `elements/Carousel.qml`, `Grid.qml`, `Channels.qml` | `T.tileImage(item)` |
| `elements/Video.qml`, `Gallery.qml`, `NowPlayingAudio.qml` | `T.artUrl(ctx, role)` |
| `elements/Xmb.qml` | `row.art`, `meta.m.logo` |
| `elements/Image.qml`, `Video.qml` | the `binding` / `role` branches of `T.imageSource` |

Neither blanket rule can be applied inside `resolve`, because content legitimately carries **both**
of the shapes a manifest must be denied:

* **remote** — provider posters are `https://…`. Refusing `://` blanks every catalog tile.
* **absolute local paths** — `MetaCache::scrapedImage()` (`native/src/core/MetaCache.cpp:268`)
  returns a path into the offline image cache *as the tile url*, and `LocalLibrary`
  (`native/src/core/LocalLibrary.cpp:63`) makes a relative NFO `<thumb>` absolute against the NFO's
  own folder. A containment check in `resolve` would blank every cached poster and every
  local-library thumbnail.

So the rule has to key off **provenance**, not off the shape of the string.

A third fact settles the policy: `native/themes2/THEME_FORMAT.md` already describes every manifest
asset as *"a path relative to the theme folder"*, *"a bundled font in the theme folder"*,
*"(relative image path)"*, *"a WAV file relative to the theme folder"*. A URL in a manifest was never
a documented feature. Enforcing local-and-contained makes the documented contract true; it does not
remove a capability any theme was told it had.

## Design

### 1. Split the funnel; delete `resolve`

`ThemeView.qml` exposes two host functions and no default:

```qml
function themeAsset(p)  { return T.themeAsset(base, p) }   // manifest-declared
function contentUrl(p)  { return T.contentUrl(p) }          // provider-supplied
```

`resolve` is **deleted** rather than kept as an alias. Every one of the ~15 call sites then has to
name which domain its string came from, and a future element cannot reach the permissive path by
habit. This is the main safety property of the change; an alias would surrender it.

### 2. The predicate lives in `Theme.js`

Not in `ThemeView.qml`, and not behind a `ThemeBridge` invokable. Two reasons, in order:

* **Testability.** `probe_themeview` evaluates the shipped `Theme.js` out of the theme2 qrc in a
  bare QML JS engine — no window, no scene, no host. A rule that lives in `Theme.js` is pinnable
  there, case by case, exactly the way `probe_theme` §8 pins the C++ half. A rule in `ThemeView.qml`
  or behind a C++ bridge is reachable only from a probe that instantiates the whole component, which
  can assert that *a* source came out empty but not the table of shapes that make it empty.
* **Cost.** These are QML property bindings, re-evaluated per tile. A grid re-resolves hundreds of
  image sources while scrolling; a C++ round-trip per binding buys nothing over a dozen lines of JS.

The cost accepted is that one rule now exists in two languages. It is contained by giving both
probes the *same case table*, and by a comment in each implementation naming the other as its twin.

**One deliberate divergence, stated rather than drifted into:** `ThemeAssetPath::resolve` judges an
absolute path by containment (an absolute path pointing inside the folder is the same file spelled
the long way, and is allowed). The JS half refuses absolute paths outright. The QML side holds the
theme directory as a `file://` URL, not a path, so it cannot do path-space containment without a
second property to keep in sync; and an absolute path in a manifest is unportable by construction —
no theme that ships to another machine can use one. The JS rule is therefore a strict subset of the
C++ rule, and both probes assert that shape so the difference is a recorded decision.

### 3. `T.themeAsset(base, p)` — the rule

Refuse, in order, and return `""` for each:

1. anything containing `://` — **the policy call**, see §5
2. anything containing `\` or `:` — refused on every platform, not just where the OS acts on it: a
   manifest is a cross-platform document, so a path that escapes on Windows must not resolve to a
   merely odd filename on Linux (this mirrors `ThemeAssetPath::resolve` exactly)
3. anything starting with `/` — absolute; see the divergence above
4. anything whose `.`/`..` segments resolve above the theme root, or to the root itself

Otherwise return `base + "/" + <cleaned relative path>`. A `..` that stays inside is normalisation,
not an escape, and is allowed — same as the C++ half.

### 4. `T.contentUrl(p)` — unchanged

Today's `resolve` body verbatim: a `://` string as-is, an absolute path as `file:///…`, anything else
joined to `base`. The relative-join branch is vestigial for content (no provider produces a relative
path) but is kept so this function is a behaviour-preserving move, not a second change riding along.

### 5. Policy: a remote URL in a manifest is refused

Manifests now arrive from a public, third-party-writable registry. A `theme.json` naming
`http://…` would give its author:

* a **beacon on every render** — the viewer's IP, user-agent and usage timing, on a machine the
  author never touched;
* a **live channel** — the bytes drawn on the user's home screen can be changed after review, so what
  the registry approved is not what gets painted;
* a **hole in offline-first**, which the codebase treats as a stated principle elsewhere (see the
  comment in `MetaCache::displayImage`).

Nothing legitimate is lost: the installer copies a theme's own files into its folder, artwork that
belongs to a theme ships with the theme, and `THEME_FORMAT.md` never documented anything else.
Content urls keep `://`, which is where remote belongs — that trust decision is governed by which
addons the user installed, not by which theme they picked.

### 6. The mixed call sites

`T.imageSource(el, ctx)` collapses four branches into one string: literal `path` (manifest),
`binding` (content), `role` (content), `fallback`-as-role (content), `fallback`-as-literal
(manifest). Its callers therefore cannot know which rule to apply.

Fix: keep provenance inside `Theme.js` by adding resolving variants that take the host:

* `T.imageUrl(el, ctx, host)` — same branch order as `imageSource`, calling `host.themeAsset` on the
  two manifest branches and `host.contentUrl` on the rest.
* `T.galleryUrls(el, ctx, host)` — the `Gallery` list: `artList` entries via `contentUrl`, the
  literal `fallback` via `themeAsset`.

`imageSource` stays as the pure "which string wins" function — `probe_themeview` pins it, and
`imageUrl` is defined in terms of the same branch order so the two cannot disagree about precedence.

A rejected manifest path yields `""` and the element shows its placeholder (or its `textFallback`).
It does **not** fall through to the next branch: falling through would render art the theme did not
ask for, which is the sanitising behaviour this whole change exists to refuse.

### 7. The two C++ callers

`ThemeEngine::loadEffect` and `MainWindow::applyThemeMusic` call `ThemeAssetPath::resolve(themeDir,
file)` instead of `QDir(themeDir).absoluteFilePath(file)`. An empty return means no sound effect for
that action and no theme-default music respectively — both already the "theme declared none" path,
so a refused path degrades to silence rather than to a broken load.

## Testing

No new probe target: the three-place registration rule in `CONTRIBUTING.md` is avoided by extending
probes that already own these surfaces.

* **`probe_theme` §8** (C++ pure predicate) — extend the section's preamble to record that theme2's
  `sounds` and `music` now come through this rule, and add the shapes those two callers actually
  produce (`sounds/move.wav` resolving, an escaping `.wav`/`music` path refused).
* **`probe_themeview`** — a new section pinning `T.themeAsset` / `T.contentUrl` / `T.imageUrl` /
  `T.galleryUrls` against the same case table as `probe_theme` §8, plus the cases that are unique to
  the split: a content `https://` url survives, a content absolute path survives, the same two
  strings in a manifest slot are refused, and `imageUrl` applies the right rule per branch.
* **`probe_navqml`** — the end-to-end half, on the real `ThemeView.qml` from the qrc: a theme whose
  `background.image` escapes renders no background source, and one naming a file inside its folder
  still does.

Gate: `BUILD_DIR=build bash native/tools/run-headless-probes.sh` must print
`ALL HEADLESS PROBES PASSED`.

## Out of scope

* `ThemeAssetPath::resolve` itself is not changed. Its absolute-path behaviour is deliberate and
  pinned; the divergence with the JS half is documented above rather than resolved by churning a fix
  that landed on another branch.
* The classic `Theme.cpp` path stays as cc39991 left it. It is still dead code; this change is about
  the path that runs.
* `Video.qml`'s mpv handoff keeps taking the raw url (mpv opens native paths, `http` and `av://`
  directly) — it is not an image source and is not routed through either function.
