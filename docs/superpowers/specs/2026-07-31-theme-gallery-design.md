# In-app theme gallery — design

Found while working #57: `RegistryBrowser` (`native/src/ui/RegistryBrowser.cpp/.h`) declares two kinds,
`Themes` and `Addons`, and the `Themes` half is dead code for a format it could not read even if reached.
This spec makes it work and reaches it from both Appearance builders.

## What is actually broken

**It is unreachable.** The only construction anywhere is
`new RegistryBrowser(RegistryBrowser::Addons, mgr_, this)` at `LibraryView.cpp:408`. The themed twin,
`MainWindow::presentAddonRegistry` (`MainWindow.cpp:4077`), is add-ons only. There is no in-app theme
gallery on either surface.

**It could not list a theme.** `fetchOne` reads the index array key `"themes"` (`RegistryBrowser.cpp:244`).
The live index at `raw.githubusercontent.com/cubman3134/everythingbox-themes/main/index.json` serves
`"themes2"`. So the browser would render zero entries and the install path would never be reached at all.

**It could not install one.** `installEntry` reads `"file"` and `"assets"` — the legacy flat colour-theme
shape — and installs into `<dataDir>/themes`, a directory nothing reads. Every live entry is
`{name, author, description, dir: "themes2/<Name>"}` with no `file` and no `assets`, so `files` comes out
empty and install bails with "Nothing to download for this entry."; `isInstalled` always returns false.
A themes2 theme is a *folder* — `theme.json` plus optional `sounds/` and `fonts/` — belonging under
`ThemeEngine::themesRoot()`, i.e. `<dataDir>/themes2/<Name>`.

Both Appearance surfaces work around all of this by telling the user to open GitHub in a browser and copy
a folder in by hand (`MainWindow.cpp:5504`, `:5554`, `:5626-5636`).

## Why build it rather than delete the kind

**Four published themes are unobtainable in-app.** The registry serves seven — Default, Channels, Grid,
Lumen, Midnight, Night, Triple. The app bundles three: Channels, Night, Triple (`AssetBootstrap.cpp:67`,
and `availableThemes()` at `ThemeEngine.cpp:247` reads that directory). Default, Grid, Lumen and Midnight
exist only in the registry.

**The documented workaround is not performable on the primary form factor.** "Open
github.com/cubman3134/everythingbox-themes and drop the folder into the themes directory" assumes a
browser and a file manager. This is a D-pad TV application with a verified Android TV build; on an onn box
there is neither. Deleting the `Themes` kind makes four of the project's own published themes permanently
undeliverable on the device class the app targets.

**Add-ons already have this on both surfaces.** Themes are the only registry-backed thing without an
in-app store. The asymmetry is the anomaly, not the gallery.

## Two findings that shape the work

**The registry copies are still the pre-#57 ones.** #57 fixed the bundled themes and added the drift gate,
but the republish step was never performed, so the registry serves Channels without `nowplayingAudio` and
a Triple with `home` alone. Filed as #131 with the hashes. It is a different repo and cannot be fixed from
this branch. It is contained here rather than worked around: see §4.

**Both existing installers flatten subdirectories.** `RegistryBrowser.cpp:371` and `MainWindow.cpp:4204`
both write to `destDir + "/" + QFileInfo(rel).fileName()`. Harmless for add-ons, whose file lists are
flat; fatal for themes, where `sounds/move.wav` would land as `<Theme>/move.wav` and every sound reference
in the theme would dangle. The theme installer must preserve relative subpaths, which is also why it must
validate them (§3).

## 1. The entry shape

Shared by both surfaces, so the two cannot disagree about what a registry says.

- **Index array key.** Accept `themes2` first, fall back to `themes`. The registry serves `themes2`;
  `themes` is what the code assumes today and costs one line to keep working. Add-ons keep `addons`.
- **Entry.** `{ name, author, description, dir: "themes2/<Name>", formFactors?: ["desktop", …] }`.
  `dir` is a path relative to the index URL's directory. `formFactors`, when present, is shown as a note
  on the row; it does not filter. Night carries it and is still installable anywhere, which matches how
  the engine treats it.
- **Folder name.** The last path segment of `dir`, validated as a single plain segment (§3). The entry's
  `name` is display text and is never used as a path.
- **Install target.** `ThemeEngine::themesRoot() + "/" + <folder>`. Not `<dataDir>/themes`, which nothing
  reads; that string in `localDirFor` is part of what made this dead code look alive.
- **`isInstalled`.** `QFile::exists(themesRoot()/<folder>/theme.json)` — the same predicate
  `availableThemes()` uses to decide a folder is a theme, so the gallery cannot claim something is
  installed that the picker will not list.

## 2. Enumerating a theme folder's files

An entry names a directory, not a file list, so the installer has to learn what is in it.

**GitHub Trees API.** From a raw index URL
`https://raw.githubusercontent.com/<owner>/<repo>/<branch>/index.json`, derive
`https://api.github.com/repos/<owner>/<repo>/git/trees/<branch>?recursive=1` and keep every `blob` whose
path starts with `<dir>/`. Their paths relative to `<dir>` are the file list.

This derives the file list from the repository itself, so it cannot drift from what the repository holds —
which is the lesson of #57, and the reason it is preferred over a `files: []` array in `index.json`. That
array would be a fourth copy of the same truth, maintained by hand, in a project that just spent an issue
on two copies disagreeing. It is also preferred over parsing `theme.json` for referenced assets
(`sounds.*`, `background.image`, element `image` and `fontFile`, particle `image`), which welds the
installer to the theme format: a new asset-bearing property in `THEME_FORMAT.md` would silently install
themes with missing files.

**Cost and failure.** One call per install, not per listing, cached per registry for the browser's
lifetime. Unauthenticated GitHub allows 60 calls an hour per IP, which a manual install action will not
approach. Three cases are not installable, and all three say so on the row rather than failing silently:

- the registry is not on GitHub (a user-added registry may be anywhere),
- the API call fails or is rate-limited,
- the response has `"truncated": true`, meaning the listing is incomplete and cannot be trusted.

In each case the entry still *lists* — the user can still read what the theme is and go install it by
hand — and Install reports that this registry cannot be installed from in-app, naming the folder. An
honest refusal, unlike today's "Nothing to download for this entry."

## 3. Safety

Every path in that tree is remote content about to become a filename.

**Validate, do not sanitize.** A path is accepted only if it is a relative path under `<dir>/` whose
segments are all plain: no `..` segment, no leading `/`, no drive letter, no backslash, no empty segment,
no reserved device name. Anything else fails the whole entry rather than being rewritten into something
that looks safe — a rewritten path is a guess about intent, and there is no benign reason for a theme to
ship one.

**Preserve subpaths.** Write to `destDir + "/" + <path relative to dir>`, `mkpath`ing each parent. This is
the flattening bug from the findings section; the validation above is what makes it safe to stop
flattening.

**Caps.** Refuse an entry whose listing has no `theme.json` at its root, more than 64 files, or any single
file over 8 MB. A registry is public and anyone can open a pull request against one.

**Atomic install.** Download into a temp sibling directory, then rename into place. An interrupted or
failed install must not leave a partial folder, because `availableThemes()` will pick up anything with a
`theme.json` and the picker will offer a theme whose fonts and sounds are missing. `AssetBootstrap`
already uses copy-to-tmp-then-rename for exactly this reason (`AssetBootstrap.cpp:75`); this is the same
discipline.

## 4. Containing the registry drift

`isInstalled` keys on folder existence, so Channels, Night, Triple and Default — every theme that is
either bundled or already on disk — show "Installed ✓" and cannot be installed over. The three drifted
registry copies are therefore never reachable from the gallery, and the one-click downgrade that #131
describes cannot happen.

This is containment, not a fix, and it is deliberately the *whole* of the containment: no "Update"
action, no version comparison, no overwrite-with-confirm. An update path is the thing that would need
#131 resolved first, and offering it now would mean offering a confirm dialog whose only available answer
is a downgrade. Updating an installed theme is out of scope (§7) and stays out until #131 closes.

## 5. Reaching it from both builders

`MainWindow::openAppearance()` has the two builders CONTRIBUTING.md describes. Both get the gallery.

**Themed.** New `MainWindow::presentThemeRegistry()`, modelled on `presentAddonRegistry`
(`MainWindow.cpp:4077`) and sharing its shape: a `reg.status` Info row that starts at "Loading…", one
Action row per entry carrying "Installed ✓" or `by <author>`, a 15-second timer so "Loading…" cannot
stick, and a `themedPanelIsTop` guard so a fetch that lands after the user navigated away is dropped.
Reached from a new `appr.browse` Action row — "Browse community themes…" — in the Appearance panel's
"Get more themes" section, above the existing `appr.gallery` row.

**Classic.** A "Browse community themes…" button in the QWidget Appearance panel, under the existing
`share` label, calling
`showDialogPanel(tr("Browse Themes"), new RegistryBrowser(RegistryBrowser::Themes, nullptr, this), …)`.
`showDialogPanel` (`MainWindow.cpp:9078`) sets `Qt::Widget` and hosts the dialog inline, so nothing
becomes a top-level window and the nav kit rule holds — the same hosting `LibraryView::browseAddons` uses
for the add-on browser.

`RegistryBrowser`'s `AddonManager*` is passed `nullptr` on this path. All three sites that touch `addons_`
(`RegistryBrowser.cpp:218`, `:287`, `:382`) already null-check or are `kind_ == Addons`-gated, so this is
safe today. One tightening goes with it: `isRemoteEntry` is currently kind-agnostic, so a theme entry that
happened to carry a `url` key would take the remote-add-on branch and try to subscribe to it as an add-on
source. Gate the remote branch on `kind_ == Addons` in both `isInstalled` and `renderEntry` — a theme entry
has a `dir`, and "remote" is a concept that only exists for add-ons.

**The GitHub link stays on both.** Contributing a theme still needs a browser, and so does a registry the
gallery cannot install from. What changes is its framing: the hand-copy instructions at
`MainWindow.cpp:5626-5636` and the `appr.customise` / `appr.community` rows stop being *the* way to get a
theme and become the way to share one, plus the fallback.

## 6. After an install

`availableThemes()` reads the directory on every call, so nothing needs invalidating — re-rendering is
enough. The themed panel rebuilds its rows so the installed entry flips to "Installed ✓" and updates
`reg.status`. The classic panel re-runs its build on `installedSomething()`, so the theme list picks up
the new folder. Neither restarts anything: a newly installed theme is selectable immediately, and applying
it goes through the existing `ThemeChoice::setForProfile` path unchanged.

## 7. Out of scope

- **Updating an installed theme**, for the reason in §4. Reopens when #131 closes.
- **Uninstalling a theme.** Separate question, and a sharper one than it looks: the bundled three are
  re-extracted by `AssetBootstrap` on every version bump, so "uninstall" would not stick for them.
- **Registry-management UI in the themed twin.** Add-on parity: `presentAddonRegistry` deliberately omits
  add/remove-registry and leaves source management to the classic browser. The theme twin matches.
- **Filtering by `formFactors`.** Shown as a note, not enforced (§1).

## 8. Gate

A source-level check in `run-headless-probes.sh`, alongside the other tree-scanning gates, asserting that
both Appearance builders reference the theme registry — the themed one via `presentThemeRegistry` and the
classic one via a `RegistryBrowser::Themes` construction. The two-settings-builders rule is the one
CONTRIBUTING.md calls out as easiest to half-do, this feature is a fresh instance of it, and the failure
mode is exactly what this spec exists to fix: a surface that silently loses its twin. The gate is cheap
and catches the regression at the moment it is written.

## Files

- `native/src/ui/RegistryBrowser.{h,cpp}` — the `dir` entry shape, `themes2` index key, tree listing, path
  validation, subpath-preserving atomic install, `isInstalled` on folder existence.
- `native/src/ui/MainWindow.cpp` — `presentThemeRegistry()`, the `appr.browse` row, the classic button,
  and the reframed instructional copy.
- `native/tools/run-headless-probes.sh` — the two-builders gate.
