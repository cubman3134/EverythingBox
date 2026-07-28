# Settings save/discard — design

GitHub issue #26. Leaving a settings screen with changes should ask whether to keep them.

## What happens today

Every settings surface is **immediate-apply**. All 34 `Settings::set*` accessors have the same body:

```cpp
store().setValue(QStringLiteral("<key>"), v);
store().sync();
```

Row handlers call those inline from their activation lambdas, in both the themed `PanelRow` builder
and the classic QWidget builder. There is no pending state, so there is nothing to discard, and Back
is indistinguishable from Save.

The consequence worth naming: a mistaken change is already permanent, and on a D-pad or remote it is
easy to bump a Choice row while scrolling past it.

`editProfilePanel` is the one existing hold-and-commit surface — values held in `shared_ptr`,
committed only on Save, Back discards.

## The approach: snapshot and restore, not buffer and flush

The obvious design is a pending map: rows stop writing, the panel renders pending-over-stored, Save
flushes. It is also the wrong one here, for a specific reason — **the rows most worth protecting are
the ones that must apply live.** Theme previews live; display mode re-lays out the surface you are
standing on. A pending map has to special-case exactly those, and then Discard is a lie for them.

So: keep immediate-apply completely unchanged, snapshot the prior state on entry, and make Discard a
restore.

This inverts the cost. Live rows need **no special case at all** — the theme previews because the
write genuinely happened. No panel changes how it writes. All 34 accessors, `ThemeChoice::setForProfile`
and the five direct `store().setValue` sites keep working untouched.

## 1. `SettingsTxn` — the core unit

New `native/src/core/SettingsTxn.{h,cpp}`. QtCore only, its own file-local `store()` (the idiom every
other core store uses), so a headless probe links it lean.

```
bool inScope(const QString& key);   // pure: is this key owned by the settings screens?

void begin();      // snapshot every in-scope key's current value
bool isDirty();    // any snapshotted key whose value now differs, or any in-scope key that appeared
void commit();     // drop the snapshot
void rollback();   // restore every differing in-scope key to its snapshot value, then run the hooks
bool active();     // is a snapshot open
```

`begin()` on an already-open txn is a no-op rather than a reset — nested panels (hub → Appearance →
theme picker) share the outermost transaction, so Discard from any depth reverts the whole visit.

## 2. The scope carve-out is the load-bearing part

A whole-ini snapshot would be a **data-loss bug**, not a nuisance: cloud sync, stats accrual, resume
positions and the download catalog all write while a settings panel is open, and a rollback would
clobber them.

`inScope` excludes keys written **outside** the settings screens. Note this is deliberately *not*
"exclude everything device-local" — `display/mode`, `roms/folder`, `library/folder` and
`emulators/root` are all device-local *and* are settings rows a user must be able to discard.

Excluded — **prefix** matches:

- Everything `CloudMerge` owns, matching `CloudSync::isPerItemStoreKey` exactly: `resume/`,
  `recent/`, `marks/`, `favorites/`, `playlists/`, `stats/`, `playstats/`, `deleted/`.
- `cloud/` — OAuth tokens written by the sign-in flow. Signing in is not a setting you discard.
- `device/` — this install's identity and one-shot migration flags.
- `pcgames/` — catalog written by the PC-game importer.
- `addon.remote.manifest.` and `addon.update.etag.` — addon caches written when a background network
  reply lands (`AddonManager::refreshRemoteManifests` / `checkAddonUpdates`, both kicked from the
  constructor). Discarding the manifest cache after `reload()` already rebuilt the source list from it
  leaves the two disagreeing until the next launch. The prefixes are long on purpose: a bare `addon.`
  would swallow the settings rows below.
- `downloads` and `downloads/` — the background download catalog, matched as an exact key **and** a
  family so a sibling like `downloadsPanel/x` is not swept up.

Excluded — **exact** key matches. Each of these is written by an async callback but sits in a group
whose other keys are genuine user-entered settings, so a prefix would make those undiscardable:

- `trakt/access`, `trakt/refresh`, `trakt/expiry` — rewritten from the `QNetworkReply::finished`
  lambda in `TraktClient::ensureValidToken`, i.e. during scrobbling, which runs while a settings panel
  is open. Trakt **rotates** the refresh token, so restoring the snapshot puts a consumed token back
  and permanently breaks the account link. `trakt/clientId` / `trakt/clientSecret` stay **in** scope.
- `ra/user`, `ra/token` — written from rcheevos' async login callback. `ra/apikey` stays **in** scope.
- `addon.stremio.seeded`, `addon.debridio.removed`, `addon.cinemeta.removed`,
  `addon.cinemeta.removed2`, `addon.torrentio.seeded`, `addon.torrentio.host.migrated` — the one-shot
  seed/migration latches from `seedDefaultStremioSources()`, the same shape as the `device/` flags.
  `addon.enabled.<id>` and `addon.remote.urls` stay **in** scope.

Everything else is in scope. `player/volume` is a deliberate keep: the player-page slider writes it,
but that surface and the settings area cannot be open at once, so it cannot move mid-visit.

Every exclusion is pinned in `probe_settingstxn` §1 **as a pair** — the excluded key out of scope, and
the neighbouring user-entered row still in it. The pairing is what stops a future sloppy prefix.

`CloudSync::isDeviceLocalKey` / `isPerItemStoreKey` are the precedent for this *shape* of
classification — a pure key predicate with a documented rationale per family. `inScope` reuses the
shape, not the contents.

Because it is pure, a probe pins the table and mutation-tests it.

## 3. Rollback re-applies side effects

Restoring a value is not enough when the setting drives visible state. After restoring, `rollback()`
runs a small named hook list:

- `display/mode` restored → `FormFactor::instance().refresh()`
- the theme key restored → re-render the current surface
- anything else the panel needs → the caller's re-render, run after `rollback()` returns

Discard must revert the *visible* change, not just the stored value. A Discard that leaves the app
looking different is the failure this design exists to prevent.

## 4. Panels gain two lines, and all thirteen are covered at once

The settings hub has thirteen screens (General, Stats, Appearance, Add-ons, Downloads, Cloud Sync,
Split Screen, RetroAchievements, Stand Alone Emulators, Libretro, BIOS Check, Input Mapping, Debug).

- `SettingsTxn::begin()` when the settings area is entered.
- On the way out, if `isDirty()`: `NavConfirm::ask(title, message, {Save, Discard, Keep editing},
  focusIndex = 0 /* Save */, cancelIndex = 2 /* Keep editing */)` → commit / rollback+re-render / stay.

Because this hangs off the shared exit path rather than each panel, both builders and all thirteen
screens are covered together. `NavConfirm::ask` already returns a button index from a nested loop, so
no new UI primitive is needed, and the nav-kit rule is satisfied without a `QDialog`.

**Leaving with nothing changed never prompts.** A prompt on every exit trains people to dismiss it.
Changing a value and changing it back counts as no change, because `isDirty()` compares values rather
than tracking edits.

Esc maps to Keep editing, so an accidental dismiss changes nothing. Save is focused: the common case
is one keypress, and neither destructive option sits under the default cursor.

## 5. Two hazards handled explicitly

**A remote bundle applying mid-panel.** `CloudSync::applySettingsJson` writes in-scope keys. If that
lands while a txn is open, a later rollback would silently undo another device's changes. So an apply
while `active()` **commits** the transaction first. Losing the ability to discard is the correct
trade against clobbering a peer. That guard lives in `CloudSync`'s translation unit, which
`probe_settingstxn` does not link, so it is pinned in `probe_cloudmerge` §16c instead — without that
case, deleting the guard passed CI.

**Masked credential rows** revert like any other key. They cannot be shown in a diff, and nothing may
special-case them into being displayed — the prompt says how many settings changed, never which
values.

## 6. Testing

`probe_settingstxn`, sentinel `SETTINGSTXN-OK`, registered in all three required places (its
`add_executable`, `run-headless-probes.sh`, and the `--target` list in `ci.yml`).

- `inScope` — every excluded family, and the device-local-but-in-scope cases (`display/mode`,
  `roms/folder`) that a naive "exclude device-local" implementation would get wrong.
- `isDirty` — clean exit, a changed value, and a value changed and changed back (must read clean).
- `rollback` — restores changed keys; **leaves an out-of-scope key written mid-txn untouched**; and a
  key that did not exist at `begin()` and was created during the txn is removed.
- `begin()` while active is a no-op, so nested panels share one transaction.
- Idempotence: `rollback()` twice equals once.

Every assertion mutation-tested: break the implementation, confirm the probe fails, revert, confirm
green. An assertion that passes under a broken implementation is not coverage.

Live verification through the `EB_UITEST` harness on a throwaway copy: change a Choice row and Back →
prompt; Discard → value and appearance both revert; Save → persists; Back with no change → no prompt;
change-and-change-back → no prompt; theme changed then discarded → the app looks as it did.

## Deliberately not in scope

- **Per-row revert.** Discard is all-or-nothing for the visit.
- **A visible diff of what changed.** The prompt states a count, not a list. Showing values would
  leak credentials.
- **Reworking `editProfilePanel`** to use the txn. It already holds and commits correctly.
