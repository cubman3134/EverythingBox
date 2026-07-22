# Item Marks + Shelves Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Playnite-class library management for all catalogs: per-profile hidden/completion/tags with pinned-tag shelves, riding the existing per-profile FavoritesStore for favorites.

**Architecture:** ItemMarks store (hidden/completion/tags + tag vocab + pinned set; SyncOffsets-style hashed keys; in-memory cache) applied at ONE aggregation seam (`HomeView::populate`) + the `renderRecents` classic path; detail verbs via the established themedDetailData/ActionRow/runThemedDetailAction pipeline; pickers from NavMenu/Osk building blocks (no multi-select overlay exists — the Tags picker is a re-presenting NavMenu loop). Spec: `docs/superpowers/specs/2026-07-21-item-marks-shelves-design.md`. **Spec amendment (this plan's authority): favorites stay in FavoritesStore** (already per-profile, already verb-wired, already has a synthetic-folder precedent) — ItemMarks does NOT duplicate a favorites bit; the spec's "Favorites row" is built FROM FavoritesStore.

**Tech Stack:** Qt 6.8.3, QSettings ini, NavOverlay kit, headless probes.

## Global Constraints

- Branch: `library/marks-shelves` off main. Standing autonomy through the merge gate.
- ANCHOR ON FUNCTION NAMES. Scout-verified seams (main@ee4d069):

| Concern | Anchor |
|---|---|
| Hidden-filter (browse + BOTH search modes) | `HomeView::populate` — filter `cat.items` before the `items_ +=` (~:3792) |
| Hidden-filter (classic Home groups) | `HomeView::renderRecents` (~:1684) + its `renderFavorites` section |
| Shelf-row injection | the `pushFolders` lambda + call-sites in `populate` (~:3686-3751); per-console `_favorites` precedent ~:3739 |
| Detail verbs | `HomeView::themedDetailData` verbs list (~:3106-3131); render `ActionRow.qml metaFor` (~:32-49); dispatch `MainWindow::runThemedDetailAction` (~:3631); state re-push idiom = the favorite branch (~:3647) |
| Item key | `MetaCache::keyFor(it)` (id-or-url) — ItemMarks hashes it internally (SyncOffsets MD5 pattern) |
| Profile-switch invalidation | `MainWindow::chooseProfile` (~:4620) — ProfileStore emits NO signal; invalidate imperatively before `openHome()` |
| Per-profile storage mirror | `PlaylistStore::plKey` / `FavoritesStore::favKey` (`"marks/<profileId or default>/..."`) |
| Pickers | `NavMenu::pick` (single-select, the playlist-picker precedent ~HomeView:1571), `Osk::getText`; NO multi-select overlay exists |

- ItemMarks is QtCore-only; linked into the APP target in ITS OWN task; probe registered runner + ci.yml; empty-key no-op; keys hashed internally; probes never touch the real ini (build-tree posture).
- Storage keys exactly: `marks/<profileId>/items/<itemHash>` (JSON: `hidden` bool, `completion` string none|inprogress|finished|abandoned|planned, `tags` string-list), `marks/<profileId>/tagVocab`, `marks/<profileId>/pinnedTags`; General toggle key `library/showHidden` (default false, PER-PROFILE? No — global app setting, hidden-ness itself is per-profile).
- Desktop suite + perf gates; live walks protect real data (throwaway profile for mutation tests; byte-identical ini restores; Weekend Picks untouched).
- Restricted profiles: marks work normally (hidden is personal preference, NOT parental control — per spec).

---

### Task 1: ItemMarks store + probe

**Files:** Create `native/src/core/ItemMarks.{h,cpp}`, `native/tools/probe_marks.cpp`; Modify `native/CMakeLists.txt` (app target + probe target), runner, ci.yml; Modify the spec (the favorites-stay-in-FavoritesStore amendment note under Decisions).

**Interfaces (Produces):**

```cpp
namespace ItemMarks {
enum class Completion { None, InProgress, Finished, Abandoned, Planned };
struct Marks { bool hidden = false; Completion completion = Completion::None; QStringList tags; };
Marks   get(const QString& key);                    // cached; empty key -> {}
void    setHidden(const QString& key, bool);
void    setCompletion(const QString& key, Completion);
void    setTags(const QString& key, const QStringList&);   // also unions new tags into vocab
QStringList tagVocab();                             // active profile's tags
void    removeTagEverywhere(const QString& tag);    // vocab + strips from all items + unpins
QStringList pinnedTags();
void    setPinned(const QString& tag, bool);
QVector<QString> itemKeysWithTag(const QString& tag);      // HASHED keys — see note
bool    anyHidden();                                // fast: does the profile have hidden items at all
void    invalidate();                               // profile switch / external change
// NOTE on shelves: hashed keys can't reverse to items, so shelf building INTERSECTS
// the current catalog's items (hash each candidate's keyFor -> membership test) — get()
// is the cache-backed hot path; document this in the header.
}
```

- [ ] Probe RED first: per-profile isolation (write profile A via a seeded ProfileStore current, switch, B sees {}), tag vocab union + removeTagEverywhere strips+unpins, completion round-trip incl. unknown-string→None, hashed-key collision independence (a/b vs a//b vs URL), empty-key no-op + junk-key absence, cache invalidation (write→get hot; invalidate→re-read), anyHidden fast-path truth. Sentinel MARKS-OK.
- [ ] Implement (JSON-per-item under the group; cache = QHash<hash,Marks> built lazily per profile). Link into app target SAME task. Suite green. Commit `feat: ItemMarks store — hidden/completion/tags per profile (marks T1)`.

---

### Task 2: hidden filtering + detail actions

**Files:** Modify `native/src/ui/HomeView.cpp/.h` (populate filter, renderRecents filter, themedDetailData verbs + state), `native/src/theme2/qml/elements/ActionRow.qml` (metaFor cases), `native/src/ui/MainWindow.cpp/.h` (runThemedDetailAction dispatch + the pickers + chooseProfile invalidation + General "Show hidden" toggle row)

- [ ] populate(): before `items_ +=`, drop items whose marks.hidden (unless `library/showHidden`); same guard in renderRecents' recents+favorites groups. `SearchAggregator` rides populate — verify with a probe-level or live check that a hidden item vanishes from search too.
- [ ] Detail verbs: `hide` (toggle label Hide/Unhide), `status`, `tags` — gated `!it.type.startsWith('_')`. ActionRow metaFor entries. Dispatch: hide → setHidden + re-push detailData (favorite-branch idiom) + refresh the underlying rows (the item may need to vanish NOW — re-run the current level's populate path; find the cheapest existing refresh); status → `NavMenu::pick` over the five states (current one check-marked in its row text) → setCompletion + re-push; tags → the re-presenting NavMenu loop: rows = vocab tags with ✓ (on item) / pin glyph, + "New tag…" (Osk::getText, non-empty, unions vocab) + per-tag toggling on pick, re-present until Cancel/Back; a "Pin/Unpin shelf: <tag>" row appears for the SELECTED tag (keep the loop simple — document the exact loop UX in the report).
- [ ] chooseProfile(): `ItemMarks::invalidate()` before openHome(). General panel: "Show hidden items" Toggle row (`library/showHidden`, setter-verbatim) + trigger a home refresh on change.
- [ ] Live (throwaway profile): hide → vanishes from rows AND search; Show-hidden → returns; status set + persists; tag create/apply/remove; profile switch isolation (real profile unaffected — verify then restore byte-identical). Commit `feat: hidden filtering + hide/status/tags detail actions (marks T2)`.

---

### Task 3: shelves + browse filter

**Files:** Modify `native/src/ui/HomeView.cpp` (pushFolders injection + the synthetic catalog for shelves), `native/src/browse/SyntheticCatalogs.h` if the existing synthetic pattern lives there, `native/src/ui/MainWindow.cpp` (browse Filter action if it lands in the themed action set)

- [ ] Favorites shelf: generalize the per-console `_favorites` precedent to a per-catalog Favorites folder row (FROM FavoritesStore, non-empty gate). Pinned-tag shelves: one folder row per pinned tag whose intersection with the current catalog is non-empty (the header-note intersection approach; measure cost on a big catalog — the marks cache makes membership O(1) per item). Hidden shelf: visible ONLY while Show-hidden is on.
- [ ] Browse Filter action (the themed browse actions zone): NavMenu — All / Favorites / status states / by tag (submenu or re-present) → filters the CURRENT level's items_ presentation (transient, not persisted).
- [ ] Live: pin a tag → shelf appears in that catalog + drills to exactly the tagged items; favorites shelf; hidden shelf under toggle; filter menu each mode. Perf spot-check on the biggest real catalog (populate with marks filtering — no visible lag; note numbers). Commit `feat: favorites/pinned-tag/hidden shelves + browse filter (marks T3)`.

---

### Task 4: close-out — spec status, gates, fable, merge

- [ ] Spec Status → complete; follow-ups recorded (filter-preset builder deferred, marks GC alongside the sync-store GC note, auto-tagging future). Full suite + perf; fable whole-branch (seams: the populate filter's interaction with prefetch/append paging + themed row indices/browseRowMap_ integrity under filtering + picker-loop UX + FavoritesStore coexistence); fix rounds; merge+push+redeploy.

## Self-Review (done at write time)

- Spec coverage: store ✅T1 (minus favorites — amended, justified); marking UI ✅T2; shelves = pinned+automatics ✅T3; hidden + Show-hidden ✅T2/T3; browse filter ✅T3; per-profile + isolation ✅T1/T2; verification ✅ per task + T4.
- The one deliberate deviation (favorites stay in FavoritesStore) is stated in BOTH the plan header and a spec amendment step (T1) — reviewers judge against the amended spec.
- Type consistency: ItemMarks::{Marks,Completion,get,setHidden,setCompletion,setTags,tagVocab,removeTagEverywhere,pinnedTags,setPinned,itemKeysWithTag,anyHidden,invalidate} used identically across tasks; hidden-filter honors `library/showHidden` in both seams.
