# Themed leaf routing: one table, and a probe that can see it

**Date:** 2026-08-22
**Follows:** a92c6dd (`fix: play a music track on the themed XMB, and stop an empty column going silent`)
**Issues:** #74 (music library), #102 (photos), #146 (OPDS books)

## The problem

Commit a92c6dd fixed three themed-layout faults that no probe could see. It left two holes behind,
and they are the same hole seen from two sides.

**1. The local-leaf list is hand-written, twice.** The themed XMB routes a media leaf's Enter through
its inline action chooser, and the chooser's Play lands in `HomeView::playThemedLeaf` — *not* in
`HomeView::activateItem`. Both functions decide "is this row a local file I should just open?" by
matching mimes and types against a list written out by hand, and the two lists are not the same list.
A kind present in `activateItem` and absent from `playThemedLeaf` falls through to `resolvePlay`,
which has no local branch, and the user is told `Nothing to play for "<title>"`.

That is not a hypothetical. The two lists have **already** drifted, in three places:

| kind | `activateItem` (classic) | `playThemedLeaf` (themed) |
|---|---|---|
| `local:video` (#8/#73) | opens it | opens it |
| `musictrack:` (#74) | queues the album | **was broken**, fixed in a92c6dd |
| `photo` (#102) | opens the viewer | **broken today** — "Nothing to play" |
| `opdsbook` (#146) | downloads with the catalog's auth, then opens | **broken today** — "Nothing to play" |

Two live bugs, of exactly the class a92c6dd fixed one instance of, sitting in shipped code with
nothing to report them. The next local media type added makes three.

**2. No probe covers themed routing at all.** All three of a92c6dd's faults were green under
`probe_nav` and `probe_themeview` before *and* after the fix. The suite could not tell the working
build from the broken one. The themed/XMB layout is the surface this app is actually used through;
classic-only verification is what let these through, and nothing about that has changed.

## The design

### A pure routing component

New unit `native/src/browse/LeafRoute.{h,cpp}`, in `namespace browse`, following the shape
`MusicCatalogs.h` established: data in, a decision out, no UI, no `Settings`, no filesystem, no scan.

```cpp
namespace browse
{
    // The routing contract for a LOCAL leaf. Declared here, used by the catalog builders that stamp
    // them, so the spelling has exactly one home.
    inline const char* kLocalVideoMime = "local:video";
    inline const char* kPhotoMime      = "photo";
    inline const char* kOpdsBookType   = "opdsbook";

    enum class LeafPlay
    {
        NotLocal,    // not a local leaf — the caller's addon/stream resolve owns this row
        OpenFile,    // hand the item over as it stands; its url IS the file
        MusicAlbum,  // queue the ALBUM named by `key`, starting at this track's file
        OpdsBook,    // fetch with the catalog's own auth first, then open
    };

    struct LeafRoute
    {
        LeafPlay play = LeafPlay::NotLocal;
        QString  key;                                    // MusicAlbum: the album key; empty otherwise
        bool isLocal() const { return play != LeafPlay::NotLocal; }
    };

    struct LocalLeafKind
    {
        const char* id;                                  // the constant's spelling
        enum Field { Mime, Type } field;                 // which of the row's two routing fields it names
        bool prefix;                                     // a keyed kind matches by prefix, not equality
        LeafPlay play;
    };

    const QVector<LocalLeafKind>& localLeafKinds();      // THE table
    LeafRoute localLeafRoute(const MediaItem& it);       // implemented BY the table, never beside it
}
```

`localLeafRoute` walks `localLeafKinds()`. Adding a local kind is **one table row**, and both dispatch
sites get it in the same commit because neither has a list of its own any more.

Guards preserved from the code being replaced: `OpenFile` and `OpdsBook` require a non-empty url and
`MusicAlbum` a non-empty key — otherwise the answer is `NotLocal` and the row falls through to the
resolve it would have taken before, rather than being claimed and dropped.

### The themed Enter gate, also pure

The `expandable || synthetic || guidance` test in `MainWindow::showThemedXmb` — the decision that
sends a row either to `browseActivate` or to the inline chooser — moves into the same unit:

```cpp
    enum class ThemedEnter { Drill, Chooser };
    ThemedEnter themedEnterFor(const QString& type, bool expandable);
```

This is fault #3 of a92c6dd (an `info` row offering Play over a sentence) made testable, and it is
the first half of the chain the probe needs: without it a probe can assert where Play *goes* but not
that a leaf reaches Play at all.

### Both call sites dispatch through it

`HomeView::activateItem` calls `localLeafRoute` where its `opdsbook` interception sits today — ahead
of the generic "a file is associated" branch, behind the `_open`, recents/downloads/favorites and
level-scoped branches, which are level decisions rather than item decisions and stay where they are.
`HomeView::playThemedLeaf` calls it where its `local:video` and `musictrack:` branches sit today.

Both switch over the same `LeafPlay`. `OpenFile` is `emit openItem(it)`, which is what the generic
branch in `activateItem` already did for `local:video` and `photo`, so the classic surface's
behaviour is unchanged. `photo` and `opdsbook` come alive on the themed surface as a consequence of
there being one table — which is the point: leaving them broken would mean writing a deliberate
exemption for a live bug.

The prefer-local lookup in `playThemedLeaf` (`LocalLibrary::index().localPathFor`) touches the
filesystem and stays in `HomeView`, below the router, untouched.

### The gate

`=== themed local-leaf routing parity ===` in `native/tools/run-headless-probes.sh`, reading
comment-stripped source. Three clauses, each naming a failure that has actually happened:

1. **`localLeafRoute` is called in both `activateItem` and `playThemedLeaf`.** Neither surface can
   quietly stop consulting the table.
2. **Every `LeafPlay` enumerator is handled in both call sites.** This is the clause that goes red on
   the pre-a92c6dd tree: `MusicAlbum` handled in `activateItem` and absent from `playThemedLeaf` is
   exactly the shipped bug, and the gate names it. Direct analogue of the existing
   `general settings builder parity` gate, including its exemption discipline — a route that
   genuinely belongs to one surface is declared with its reason, and a stale exemption fails.
3. **Every `k*` constant in the marked local-leaf block in `LeafRoute.h` appears in the table in
   `LeafRoute.cpp`.** Declaring a kind and forgetting to route it fails.

Like the settings-parity gate, this one reads two named files. Its first check is that
`browse::localLeafRoute` is still *defined* in the file it just read, so moving the unit cannot leave
the gate green while asserting nothing.

### The probe

`native/tools/probe_leafroute.cpp`, sentinel `LEAFROUTE-OK`. Four sections:

* **§1 the themed Enter gate.** A container (`expandable`) drills; a synthetic `_`-prefixed row
  drills; an `info` guidance row drills (and is inert there, which is why it must not reach the
  chooser); a real leaf opens the chooser.
* **§2 the table.** Every kind in `localLeafKinds()` routes to a non-`NotLocal` play; a `MusicAlbum`
  key round-trips through `musicKeyOf`, including an album key containing `':'`; an empty url or key
  yields `NotLocal` rather than a claimed-and-dropped row.
* **§3 the end-to-end claim the suite was missing.** Rows built by the **real** catalog builders —
  `localLibraryCatalog`, `photosFolderCatalog`, `musicAlbumCatalog`, `opdsCatalog` — chained
  `themedEnterFor` → `Chooser` → `localLeafRoute` → a player. Read out loud: *a local leaf activated
  through the themed path reaches a player.* Every row of every local builder, not a hand-picked one.
* **§4 the negative.** A remote catalog row (a Stremio-shaped movie with a url) answers `NotLocal`,
  so the router cannot swallow rows the addon resolve owns. Without this, §2 and §3 are satisfiable
  by a router that says `OpenFile` to everything.

Registered in all three places CONTRIBUTING.md requires: `add_executable` +
`target_link_libraries` in `native/CMakeLists.txt`, the runner loop in
`native/tools/run-headless-probes.sh`, and the `--target` list in `.github/workflows/ci.yml`.

`native/tools/leafroute-mutants.json` drives `native/tools/mutate.py` over the assertions: break each
table row, each gate clause's subject, and the `themedEnterFor` guidance arm, and show each goes red.

**One deliberate CMake exception.** `probe_leafroute` links both `SyntheticCatalogs.cpp` and
`MusicCatalogs.cpp`. The comment at `native/CMakeLists.txt` keeps those apart on purpose, so that
`probe_browse` / `probe_iptv` / `probe_locallib` do not inherit TagLib. This is the one probe whose
claim spans *all* local leaf kinds, so it is the one probe that has to link both — the same reasoning
`probe_browse` already uses to link `PcGameRemap` ("the only probe that can build both sides, so it
links the remap purely to assert they agree"). The CMake comment says so at the target.

## Found while verifying: the Photos category has no themed home at all

The live pass could not reach a photo leaf, because there is no Photos category on the themed XMB.
`core::mediaCategory` has filed type `photo` under its own `"photos"` bucket since #102, but
`HomeView::categoryItems()` emits only the four buckets `{video, game, audio, reading}` and
`categoryMeta` knows only those four — so the Photos tab has existed in the classic grid, and had no
themed category, for as long as the feature has existed. Same family as the routing faults: works on
the layout nobody runs, absent on the layout everybody does.

Fixed here, because the themed photo route this change adds is otherwise unreachable and therefore
untestable — shipping it would be shipping dead code. Three edits: the bucket key added to
`categoryItems`' ordered list, a `photos` entry in `categoryMeta` (name and accent), and a `photos`
arm in `Xmb.qml`'s glyph painter. The last one matters more than it looks: that painter's final `else`
is the **settings gear**, so a bucket with no arm of its own does not render glyph-less — it silently
wears another category's icon. The comment at the painter now says so.

## Out of scope

* `HomeView::browseItems`' info-row hold-back (fault #2 of a92c6dd) stays untested. It reads
  `MetaCache`, `MetaOverrides` and `typeColor`, so pinning it means extracting a fourth thing; the
  themed Enter gate in §1 covers the half that decides what an `info` row *does*.
* The other doubled panels CONTRIBUTING.md warns about. This gate covers leaf routing only.

## Success criteria — as met

* `BUILD_DIR=build bash native/tools/run-headless-probes.sh` ends `ALL HEADLESS PROBES PASSED`, with
  `PASS: probe_leafroute` and
  `PASS: themed local-leaf routing parity (4 route(s), 3 declared kind(s), both dispatch sites on the table)`.
* `native/tools/mutate.py --spec native/tools/leafroute-mutants.json` → **13 KILLED, 0 SURVIVED,
  0 NOT APPLIED**.
* The new coverage sees the bug it was written for: the `gate-themed-arm-removed` mutant restores the
  tree exactly as it shipped before a92c6dd — a local route handled in the classic grid and missing
  from the themed one — and the gate goes red on it. `table-musictrack-dropped` does the same to the
  probe.
* Live, on Triple, through the uitest channel: a photo tile's Enter opens the inline chooser
  (`themedFocus: "action:0"`), and its Play opens the photo viewer at page 1/2 of the folder. The
  guidance row in an empty Music category takes the ordinary path and is inert — Enter leaves focus on
  the row rather than opening a chooser over a sentence.
* Not live-verified: the OPDS book leaf, which needs a reachable OPDS catalog to stand up. Covered by
  `probe_leafroute` §3 over the real `opdsCatalog` builder and by the `table-opdsbook-dropped` mutant,
  and its route is the same `openOpdsBook` the classic grid has always used.
