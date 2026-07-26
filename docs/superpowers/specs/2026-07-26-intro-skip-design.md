# Intro / Credits Skip (roadmap #10) — Design

**Date:** 2026-07-26
**Status:** Draft — approved through brainstorming; awaiting user spec review before plan.
**Origin:** Roadmap #10 ("intro-skip"). Greenfield: nothing named intro/skip/credits/recap/outro exists
anywhere in `native/src` or `docs`.

## Prior art this borrows from

[Jellyfin](https://jellyfin.org/docs/general/server/metadata/media-segments/) settled on a three-way split —
**detection** (pluggable providers), **storage** (a core Media Segments API of *typed* ranges: Intro, Outro,
Recap, Preview, Commercial), and **action** (each client decides skip-button vs auto-skip). It ships both a
chapter-derived provider and the fingerprinting [Intro Skipper](https://github.com/intro-skipper/intro-skipper).
[Kodi](https://kodi.wiki/view/Edit_decision_list) has no detection at all, but reads `.edl` sidecars —
`[start] [end] [action]`, actions `0`=cut, `1`=mute, `2`=scene marker, `3`=commercial break.

This design takes the split and the typed store. It does **not** take Jellyfin's execution model: Jellyfin
fingerprints server-side as a scheduled batch job over the library, and MMV is a client with no daemon.

## What already exists (do NOT rebuild)

- **Player chrome**: `mediaControls_` (`MainWindow.cpp:655-715`), a `QFrame` child of `player_` with the
  transport bar, revealed by `revealMediaControls()` (`:2227-2238`) on a shared 4 s `controlsHideTimer_`.
  `streamIssueBtn_` (`:760-774`) is the precedent for a *conditional* extra button over the video.
- **`Notifier::playerNotice(msg, ms)`** — a centred transient label over the player (informational only).
- **Player input**: the player does **not** grab input; keys are handled inline at `MainWindow.cpp:2037-2055`.
  Touch via `handlePlayerTouch`/`onPlayerTap` (`:2288-2340`).
- **Seeking**: `MpvWidget::setPosition(double)` (`MpvWidget.cpp:446-449`) sets `time-pos` absolutely.
  `seekRelative` (`:439`) exists too. The `time-pos` DOUBLE observer already drives the seek slider.
- **Chapters, partially**: `nextChapter()`/`prevChapter()` (`MpvWidget.cpp:451-462`, relative only) and a
  `chapterCountChanged(int)` signal off the `chapters` INT64 observer (`:256-259`). **There is no
  `chapter-list` access** — no per-chapter time or title anywhere in the tree.
- **Next-episode**: `tryPlayNextEpisode` (`MainWindow.cpp:2536-2570`) — splits `subCtx_.imdbStreamId` into
  show/season/episode, tries local file first, then `resolveStreamByImdb`, then the next season.
- **Episode identity**: `LocalLibrary::parseFile(path)` (pure, filename-only), `showKeyFor(e)` =
  `seriesImdbId` else `"name:"+show.toLower()`, and `MediaItem::imdbStreamId` = `"tt…"` or `"tt…:S:E"`.
- **Store precedents**: `SubtitleCache` (device-local JSON under `AppPaths::dataDir()`) and `SyncOffsets`
  (per-key numeric with a global fallback). `ItemMarks` is **not** usable — its payload is a fixed struct
  (`hidden`/`completion`/`tags`/`updatedAt`) with no free-form field for a range.

## Decisions taken in brainstorming

| Question | Decision |
|---|---|
| Detection | Three providers. Precedence as designed was **`.edl` → chapters → learned**; **as shipped it is `exact-season learned` → `.edl` → chapters → `inherited learned`** — see "Tier order, revised during implementation" below |
| Action | A timed on-screen chip; a setting flips it to silent auto-skip |
| Types acted on in v1 | **Intro and Credits** (Recap/Commercial are stored, not acted on) |
| Learned-range scope | **Per season, falling back to the series' most recent mark** |

## Design

### 1. `MediaSegments` — pure model, parsers, and tracker (`native/src/core/MediaSegments.{h,cpp}`)

```cpp
namespace MediaSegments
{
    enum class SegmentType { Intro, Credits, Recap, Commercial };

    struct Segment {
        double      start = 0;
        double      end   = 0;
        SegmentType type  = SegmentType::Intro;
    };

    // A series-scoped identity for the learned tier. seriesKey is empty when nothing identifies a show.
    struct Key { QString seriesKey; int season = 0; };

    // One mpv chapter. Declared HERE, not in MpvWidget: core must not depend on the video layer, and
    // declaring it here is what lets probe_segments test fromChapters() without linking libmpv at all.
    struct Chapter { double time = 0; QString title; };

    // --- providers (pure) ---
    // Kodi .edl: "[start] [end] [action]", whitespace-separated. Times: plain seconds ("5.3"),
    // "HH:MM:SS.sss", or "#<frame>". fps<=0 means frames cannot be converted, so frame-form lines drop.
    QVector<Segment> parseEdl(const QString& text, double duration, double fps);
    QVector<Segment> fromChapters(const QVector<Chapter>&, double duration);

    // --- combination ---
    // Per-TYPE precedence: for each type, the first list that supplies one wins. NOT whole-list precedence,
    // which would let an .edl carrying only a commercial break suppress a chapter-derived Intro.
    // Tier order revised during implementation — see the section below on why exactLearned is FIRST and
    // inheritedLearned is LAST.
    QVector<Segment> resolve(const QVector<Segment>& exactLearned,
                             const QVector<Segment>& edl,
                             const QVector<Segment>& chapters,
                             const QVector<Segment>& inheritedLearned);

    // --- identity ---
    // imdbStreamId ("tt…:S:E") first; else the filename via LocalLibrary::parseFile + showKeyFor.
    Key keyFor(const QString& imdbStreamId, const QString& localPath);
}
```

**EDL typing.** The format carries no notion of "intro", so skippable ranges (actions `0` cut and `3`
commercial break) are typed by **position**, with these exact constants:

- `kCreditsTailS = 60` — a range ending within 60 s of `duration` → **Credits**.
- `kIntroWindowS = 900` — otherwise, the **first** skippable range starting before 15:00 …
- `kIntroMaxLenS = 300` — … and lasting ≤ 5:00 → **Intro**.
- Anything else → **Commercial** (stored, never acted on in v1).
- `kMinSegmentS = 5` — ranges shorter than 5 s are dropped as noise at parse time.

**The Credits test runs first, and the order is load-bearing.** In a short file a single range can satisfy
both rules (starts before 15:00 *and* ends near the end); typing it as Intro would make the chip offer to
skip the whole remaining episode. Credits-first makes the overlap resolve the harmless way.

Actions `1` (mute) and `2` (scene marker) are parsed and discarded — they are not skips.

**Chapter typing.** The chapter title is normalized (lowercased, punctuation → spaces, collapsed) and matched
**on word boundaries** against phrase sets:

- Intro: `intro`, `opening`, `opening credits`, `opening titles`, `titles`, `theme`, `op`
- Recap: `recap`, `previously`, `previously on`
- Credits: `credits`, `end credits`, `ending`, `outro`, `closing credits`, `ed`

Word-boundary matching is load-bearing: substring matching makes every documentary chapter named
**"Introduction"** a phantom intro, and `op`/`ed` (anime rip convention) would match inside half the words in
English. A chapter's range runs from its own time to the next chapter's time (or `duration` for the last).

**`SegmentTracker`** — a small class in the same header, holding the per-playback state:

```cpp
class SegmentTracker {
public:
    void reset(QVector<Segment> segments);
    // Returns the segment to offer on entering it, else nullopt. Offers each segment at most once,
    // until a backward seek to before its start re-arms it.
    std::optional<Segment> onPosition(double t);
};
```
This is a class rather than inline `MainWindow` code because it owns the consumed-set and the re-arm rule —
the two places this feature can plausibly get subtle bugs — and as pure logic it is fully probe-testable with
no player, no file, and no clock.

### 2. `SegmentStore` — the learned tier (`native/src/core/SegmentStore.{h,cpp}`)

Device-local JSON at `AppPaths::dataDir()/segments.json`, modeled on `SubtitleCache`:

```cpp
class SegmentStore {
public:
    explicit SegmentStore(QString filePath);
    void load();  void save() const;

    // This season's marks, else the DERIVED nearest-season fallback. Empty when nothing is learned.
    QVector<MediaSegments::Segment> lookup(const QString& seriesKey, int season) const;
    // Only this season's marks, never the fallback — what makes an explicit mark outrank a detector.
    QVector<MediaSegments::Segment> lookupExact(const QString& seriesKey, int season) const;
    // Writes exactly ONE key. See below: the original "write a bare copy too" design broke forget().
    bool put(const QString& seriesKey, int season, const MediaSegments::Segment&);
    bool forget(const QString& seriesKey, int season);
};
```
`put` replaces any existing segment **of the same type** at that scope and leaves other types alone, so
marking credits never clobbers a learned intro. Both mutators return `false` on a failed disk write; the
caller reports that rather than claiming the mark was saved.

**Revised during implementation — the store writes one key and derives the fallback.** The original design
had `put` write both the season key *and* a bare series key meaning "most recent mark". That made `forget`
impossible: it removed only the season key, and `lookup` served the bare copy straight back, so there was no
call sequence that cleared a mark. A first attempt tagged each row with the season that wrote it; that in
turn regressed inheritance, because forgetting the season that happened to write last wiped the entry other
seasons were relying on. The shipped store keeps **only** `"<seriesKey>|s<N>"` entries and computes the
fallback at lookup time from the **nearest** season — largest below the requested one, else smallest above,
with a season-unknown mark ranking last. Nearest rather than highest because if seasons 1-3 share an opening
and season 5 changed it, an unmarked season 2 should inherit season 1's mark, not season 5's.

### 3. `MpvWidget::chapters()`

```cpp
struct Chapter { double time; QString title; };
QVector<Chapter> chapters() const;
```
Reads `chapter-list/N/time` and `chapter-list/N/title` — a direct analogue of the existing `tracksOfType()`
(`MpvWidget.cpp:487+`), which `snprintf`s indexed property names and calls `mpv_get_property_string`.

### 4. The chip (`MainWindow`)

`skipChip_`, a focusable `QPushButton` child of `player_`, styled like `streamIssueBtn_` but with its **own**
`QTimer` (`kChipMs = 8000`) rather than the shared `controlsHideTimer_`, since it must outlive a chrome hide.

- Intro → label "Skip Intro", action `player_->setPosition(seg.end)`.
- Credits → label "Next Episode", action `tryPlayNextEpisode()`.
- Dismissal: pressing it, the segment ending, or the timer expiring. Once dismissed it does not reappear for
  that segment this playback.
- Input: click/tap, the **`S` key** in the player `keyPressEvent` switch (free — the player currently binds
  Left/Right/Up/Enter/Space/F12/`[`/`]`), and membership in the transport focus chain **while visible** so
  the controller ring can reach it. `S` does nothing when no chip is showing.

**This is deliberately not a `NavOverlay`.** Every `NavOverlay` grabs all input (keyboard grab + `NavContext`
routing, LIFO stack, topmost owns everything), which is precisely wrong for a non-modal prompt over live
video. The nav-kit rule targets dialogs and top-level windows; the precedent followed here is
`mediaControls_`/`streamIssueBtn_`, existing non-modal child widgets of the classic-QWidget player page.

### 5. Learning flow

The **`I` key** during playback opens a "Skip segments" `NavMenu` (modal is correct here — the user invoked
it) offering **"Mark intro start"** / **"Mark intro end"**, **"Mark credits start"**, and **"Forget marks for
this season"**.

**Not the Esc menu.** `showEscMenu` (`MainWindow.cpp:1163`) is the *app-level* pause menu raised from
`HomeView::backRequested`; during playback Escape falls through to the unified `goBack()` and exits to home
(`MainWindow.cpp:2054`). There is no player Esc menu to hang this on. `I` is free (the player binds only
Left/Right/Up/Down/Enter/Space/F12/`[`/`]`) and is mnemonic; `M` was rejected because mute is the near-
universal meaning of that key in a video player and MMV would be surprising to anyone who reached for it.

Because a bare keystroke is undiscoverable, the Playback settings section also carries a static `info` row
naming both keys — that is the row's whole job, so the feature is findable where its toggles already are.

- Intro: "Mark intro start" at the current position, then the menu reopens offering "Mark intro end"; on the
  second mark the range is stored and `playerNotice` confirms *"Intro remembered for Season 2"*.
- Credits: one mark; the range is `[position, duration]`.
- When `seriesKey` is empty the entries are **shown disabled with the reason** ("no series information for
  this file"), never silently inert.

### 6. Settings (both builders)

`Settings::skipSegments()` (default **on**) and `Settings::skipSegmentsAuto()` (default **off** = show the
chip), plus the `info` row naming the `S` and `I` keys. Added to the **Playback** section of the themed
builder (`MainWindow.cpp:8208-8221`, where `pb.autonext` lives) **and** the classic QWidget Playback block
(`:8633-8640`). A setting added only to the QWidget builder is unreachable for default users.

### Tier order, revised during implementation

The design above put the learned tier **last**, on the reasoning that the cheapest and most exact sources
should win. Implementation review showed that makes a user's own mark **inert on any chaptered file**, while
the app reports it as saved: the chip offers a chapter-derived intro, the user judges the range wrong and
marks their own, the notice says "Intro remembered for season 1", the re-resolve still returns the chapter
range — and nothing they can see changes. Worse, they are then re-offered the same wrong range.

The shipped order splits the learned tier in two:

```
exact-season learned  →  .edl  →  chapters  →  inherited learned
```

- A mark **for the season being watched** outranks everything. It is the most explicit signal available, it
  is the only tier the user can correct, and it is always aimed at a specific detector's mistake.
- A mark **inherited** from another season via the nearest-season fallback stays last, so one season's
  hand-mark can never override another season's perfectly good chapters.

`SegmentStore::lookupExact` (this season only) and `lookup` (with fallback) back the split; the caller
derives the inherited set as "lookup minus the types lookupExact supplies".

## Data flow

```
open item ─→ arm segCtx_ {seriesKey, season, filePath}      (imdbStreamId, else filename)
                     │
onDuration (duration known) ─→ .edl sidecar ─┐
                               chapters()   ─┼─→ resolve() ─→ SegmentTracker::reset()
                               store.lookup ─┘
                     │
time-pos observer ─→ tracker.onPosition(t) ─→ segment? ─→ off:   nothing
                                                        ─→ auto:  setPosition(end) + playerNotice
                                                        ─→ chip:  show for 8 s
```

The `.edl` is read only for local files (`QFileInfo(path).completeBaseName() + ".edl"` in the same folder);
streams have no sidecar. Gathering happens once per file load, not per tick.

## Error / edge handling

| Situation | Behavior |
|---|---|
| No `seriesKey` (a movie, unparseable filename) | `.edl` + chapters still work; the learn tier is unavailable and its menu entries are disabled **with the reason shown** |
| Malformed `.edl` line (wrong field count, unparseable time) | Drop that line, keep the rest. A bad sidecar never breaks playback |
| `.edl` frame form (`#123`) with unknown fps | Drop those lines; the rest of the file still applies |
| `end <= start`, negative, or beyond `duration` | Dropped at parse |
| Range shorter than `kMinSegmentS` (5 s) | Dropped as noise |
| `duration` unknown / 0 | Credits classification skipped entirely; Intro still works |
| Stream (no local bytes) | Chapters + learned tiers only |
| Chapter titled "Introduction" | Must **not** match Intro — word-boundary matching, asserted in the probe |
| Segment starts at 0 | Chip appears immediately on play |
| Chip ignored until it expires | Hides; does not nag again for that segment this playback |
| Backward seek to before a consumed segment | Re-arms it, so scrubbing back and replaying re-offers the skip |
| Auto-skip on a wrong learned range | `playerNotice` names what was skipped, so the cause is visible and the setting is findable |
| Two providers disagree | Per-**type** precedence (exact-season learned > `.edl` > chapters > inherited learned); never a merge |
| A user marks a range on a file that already has chapters | The mark wins — see "Tier order, revised during implementation" |
| Credits chip with no next episode | `tryPlayNextEpisode` already handles the finale case with its own notice |

## Verification

- **`probe_segments`** (new, pure, RED-first, sentinel `SEGMENTS-OK`):
  - `parseEdl`: all three time forms; all four action codes; frame form with and without fps; malformed
    lines, blank lines, wrong field counts; `end<=start`; ranges past `duration`; the sub-5 s drop.
  - EDL position typing **at its exact boundaries**: a range starting at 899 s vs 901 s, one 299 s vs 301 s
    long, one ending 59 s vs 61 s before the end; and the **overlap case** — a range satisfying both rules
    must type as Credits, not Intro.
  - `fromChapters`: each phrase set; case and punctuation variation; the **"Introduction" negative**; last
    chapter running to `duration`.
  - `resolve`: per-type precedence — an `.edl` carrying only a Commercial must not suppress a chapter Intro.
  - `SegmentTracker`: enter-once, ignore-while-inside, re-arm on backward seek, no offer when disabled.
  - `SegmentStore`: round-trip, season hit, season-miss → series fallback, same-type overwrite,
    different-type coexistence, missing file.
  - `keyFor`: the `imdbStreamId` route, the filename route, and the no-identity case.
- **Live:** an episode with an `.edl` sidecar, one with named chapters, and a plain file learned end-to-end
  (mark → replay → chip appears → skip lands at the right place); the auto-skip setting; a credits chip
  handing off to the next episode; and a second season inheriting the series-level fallback.
- Suite + app compile. No perf run — this rides the existing `time-pos` observer and adds one comparison per
  tick against a list that is almost always empty.

## Non-goals

- **Audio fingerprinting.** This design is precisely what a fourth provider would plug into; it is not built.
- DVR commercial skipping (Commercial segments are stored, not acted on), and `.edl` mute/scene actions.
- A timeline editor for reviewing or nudging stored ranges.
- Syncing learned ranges via Drive (`segments.json` is device-local, like `subtitles.json`).
- Changing chapter *navigation* (`nextChapter`/`prevChapter`), the autoplay-next gate, or anything about how
  `subCtx_` is armed — the segment context is its own struct precisely so the subtitle system is untouched.
