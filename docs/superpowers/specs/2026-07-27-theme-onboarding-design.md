# Theme onboarding — design

Roadmap task #57. Three asks: force a theme pick for new profiles instead of defaulting silently,
cut the shipped theme set to Triple + Channels, and give both onboarding and Appearance a real
preview.

## Why this isn't a small change

Two facts found while exploring reshape the work.

**The themed-home theme is not per-profile.** `themedHome/theme` is a single global key. The classic
colour theme *is* per-profile (`theme/<profileId>`, `Theme.cpp:114`), so the two systems disagree.
"Force a pick for new profiles" is meaningless until the themed key is scoped the same way.

**Cutting themes strands nobody.** `AssetBootstrap` uses `copyTreeOverwrite` — additive, never
deletes (`AssetBootstrap.cpp:65`) — and desktop updates unzip over the install folder. A user on
Grid keeps Grid forever. So the risk isn't deletion; it's that `"Default"` is hardcoded as both the
setting default and `availableThemes()`'s empty-fallback, and `Default` is one of the themes being
cut.

**The read pattern is duplicated six times.** `store().value("themedHome/theme", "Default")` followed
by `if (!themes.contains(x)) x = themes.value(0, "Default")` appears at `MainWindow.cpp` lines 4013,
4021, 4678, 4917, 5000 and 8017, with writes at 4056, 4814, 4933, 5065 and 5163 (three of those
writes are the theme-cycler hotkey). Changing the key means changing all eleven sites correctly, or
absorbing them into one unit. This spec does the latter — that consolidation is what makes the rest
safe.

## 1. ThemeChoice — the per-profile core

New pure unit `native/src/core/ThemeChoice.{h,cpp}`. Qt-core only (QSettings over the shared
portable ini, same posture as the other core stores), so it links into a headless probe. It becomes
the **only** reader and writer of the theme key.

```
namespace ThemeChoice {
    QString keyFor(const QString& profileId);   // "themedHome/theme/<id>", "<id>" empty -> "default"
    QString forProfile(const QString& profileId);              // stored value, "" when unset
    void    setForProfile(const QString& profileId, const QString& folder);

    // Does this profile still owe us a pick? True when nothing is stored, OR the stored folder is
    // no longer installed (the user's theme was deleted from disk).
    bool    needsPick(const QString& stored, const QStringList& installed);

    // What to actually render. Never returns a folder that isn't installed.
    QString resolve(const QString& stored, const QStringList& installed);
}
```

`needsPick` and `resolve` take `stored` and `installed` as arguments rather than reading globals, so
the probe pins the decision table with no filesystem and no ini.

**`resolve` ordering**, in full:

1. `stored` if it appears in `installed`.
2. `kFallbackTheme` (`"Triple"`) if it appears in `installed`.
3. `installed.first()` if `installed` is non-empty.
4. `""` — no theme is installed at all. Callers already handle this: `MainWindow.cpp:3951` covers
   "themed home is on but no theme exists on disk".

Step 3 matters. A user who deleted Triple and kept only a community theme must land on that theme,
not on a fallback that isn't there.

The six read sites collapse to `ThemeChoice::resolve(...)`; the five write sites to
`ThemeChoice::setForProfile(ProfileStore::currentId(), folder)`. No call site keeps a literal
`"Default"`.

### Migration

One pass, run once at startup, guarded by a device-local flag so it is idempotent. It folds two
moves together:

- **Global → per-profile.** If the legacy global `themedHome/theme` exists, write its value to every
  profile in `ProfileStore::list()` that has no per-profile value yet. Then remove the global key.
- **`XMB` → `Triple`.** Any stored value of `XMB` (global or per-profile) becomes `Triple`, matching
  the folder rename in §2.

Both are expressed as a pure function over (legacy global, profile list, existing per-profile map)
returning the map to write, so the probe pins the table rather than the side effects.

Consequences, stated deliberately:

- Nobody's appearance changes on update.
- No existing profile is forced into a pick — migration gives every one of them a stored value.
- A profile whose migrated theme was cut (Grid, Lumen, Midnight, Default) keeps it, because the
  folder is still on disk. `needsPick` returns false. This is intended: the cut governs what ships,
  not what a user already has.

The flag is device-local (it describes work done against *this* install's ini), so it joins the
`isDeviceLocalKey` carve-out in `CloudSync.cpp:486` alongside `onboarding/done`. The theme keys
themselves are **not** device-local and therefore sync — a deliberate match to the classic
`theme/<profileId>`, and the reason a restored profile arrives already themed.

## 2. The cut

`native/themes2/` ships exactly two theme packages:

- `Channels` — unchanged.
- `Triple` — the folder currently named `XMB`, renamed. Its `theme.json` already declares
  `"name": "Triple"`, and the community registry already uses the folder name `Triple`, so the local
  tree is the odd one out. A folder name is a stored id, so the rename is only safe because the
  migration in §1 carries it.

`Default`, `Grid`, `Lumen` and `Midnight` are deleted from `native/themes2/`. They remain in the
`cubman3134/everythingbox-themes` registry, so they move from bundled to downloadable rather than
disappearing. The Appearance screen already links to that gallery; no copy change is needed beyond
not implying they're built in.

Two literals change from `"Default"` to `"Triple"`:

- `ThemeEngine::availableThemes()`'s empty-fallback (`ThemeEngine.cpp:233`).
- The fallback constant now living in `ThemeChoice` (`kFallbackTheme`).

Android and iOS pick this up for free — both stage `native/themes2` wholesale into the package
(`CMakeLists.txt:773`, `:867`).

## 3. ThemePickerHost — one surface, two entry points

New `native/src/theme2/ThemePickerHost.{h,cpp}` plus `native/src/theme2/qml/ThemePicker.qml`, a
sibling of `ThemedPanelHost`. A persistent stack page constructed in the `MainWindow` ctor, like the
panel host, so it can present **pre-home** — before `openHome()` has built anything.

Layout: theme list on the left, live preview on the right.

- The list is one NavGraph zone, so D-pad, controller and mouse all drive it through the existing
  Nav Contract.
- The preview is a real `ThemeEngine::buildView` of the highlighted theme, rebuilt on selection
  change. This is the same mechanism the classic Appearance panel already uses
  (`MainWindow.cpp:5149`), so it is proven, self-maintaining, and gives community themes previews
  for free.
- **The preview is registered in no nav zone and is `Qt::NoFocus`.** A live QML view inside a nav
  surface will otherwise take the cursor and strand the user in a preview they cannot leave. This is
  the single most important constraint in this section.

Preview sample data is the four synthetic categories the classic panel falls back to (Video, Games,
Audio, Reading), extracted to one shared helper. At first run there is no library and no `home_`,
so the synthetic path is the *only* path that works there — sharing it means the onboarding preview
and the Appearance preview cannot drift.

Two entry modes:

| Mode | Back | Used by |
|---|---|---|
| `mustChoose` | quit-confirm, no escape (`quitConfirmFromStartup`) | onboarding |
| normal | returns to Appearance | Appearance |

`mustChoose` reuses the profile picker's existing no-escape contract rather than inventing a second
one.

**Classic (non-QML) builds** have no themed surface. `presentOnboardingChoice` already establishes
the pattern — it falls straight through to the unthemed path — and the forced pick does the same:
a classic build resolves through `ThemeChoice::resolve` and never presents a picker. The classic
Appearance panel keeps its own list+preview.

## 4. The forced pick

Injected at `chooseProfile()` (`MainWindow.cpp:5692`), the one chokepoint where a profile becomes
current — it serves both the startup picker and the runtime profile switcher.

```
ProfileStore::setCurrent(id);
ItemMarks::invalidate();
ConsumptionStats::invalidate();
if (themed && needsPick(...))  present the picker (mustChoose); on pick -> store, then openHome()
else                            openHome();
```

Keying off the profile rather than a device flag is what makes this correct for the actual ask: a
*second* profile created months later gets its own forced pick, because it has no stored theme. A
migrated profile never does. A restored profile never does, because the theme key syncs.

`maybeOfferTvMode()` stays scheduled after `openHome()` on both branches, so the TV-mode offer keeps
landing on the home screen and not over the picker.

## 5. Appearance

**Themed panel:** the `appr.theme` Choice row becomes an Action row (`Theme…`) that opens
`ThemePickerHost` in normal mode. This removes the current pretence that recolouring the settings
panel is a preview (`MainWindow.cpp:5069`) and replaces it with the real thing.

**Classic panel:** keeps its existing list and preview pane, rewired to `ThemeChoice`. Per the
standing rule, a user-facing setting must be reachable in both builders.

## 6. Proof

**`probe_theme`** (new, headless, no Qt GUI). Sentinel `THEME-OK`. Asserts:

- `keyFor` — including the empty-profile-id case.
- `needsPick` — unset, set-and-installed, set-but-uninstalled.
- `resolve` — all four ordering steps, including "stored theme gone, fallback gone, one community
  theme installed" landing on the community theme.
- The migration table — global-only, per-profile-already-set (not overwritten), `XMB`→`Triple`,
  empty profile list, and idempotence (running twice equals running once).

Every assertion is **mutation-tested**: break the implementation, confirm the probe fails, revert,
confirm green. An assertion that passes under a broken implementation is not coverage.

The probe is registered in all three required places — its `add_executable`, `run-headless-probes.sh`,
and the `--target` list in `ci.yml` — or it silently never runs.

**Live verification** through the `EB_UITEST` harness against a throwaway portable copy (never the
real install, never the real ini, `cloud/*` and `sync/*` stripped):

1. Fresh profile → the picker appears before the home screen and cannot be escaped.
2. Picking a theme → that theme renders; the key is written under that profile's id.
3. A second new profile → gets its own forced pick.
4. A profile seeded with a legacy global value → migrates, no pick, appearance unchanged.
5. A profile seeded with `XMB` → resolves to `Triple`.
6. Appearance → `Theme…` opens the picker, Back returns to Appearance, cursor never lands in the
   preview.

## Deliberately not in scope

- **An in-app theme installer.** Themes are still added by dropping a folder in and the gallery is
  still a browser link. Worth doing; not this task.
- **Re-homing the classic `Theme`/`ThemeStore` system.** It is already per-profile and correct.
- **More shipped themes.** Cutting to two makes the forced pick a choice between two, which is thin
  for a screen that blocks first launch. The fix for that is shipping more themes, not removing the
  step — noted here so the trade-off is on the record rather than rediscovered later.
