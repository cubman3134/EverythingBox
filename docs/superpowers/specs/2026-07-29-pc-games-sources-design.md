# PC games: one folder, many sources — design

Replace the per-launcher PC folders with a single **PC Games** folder where each game is one entry,
and the launcher or download you use is a **source** you pick — the same shape the video stream picker
already has.

## Where we are

Four launcher libraries, all offline-local (registry / manifest scans), each with its own id shape and
its own launch mechanism:

| Library | Id | Launch |
|---|---|---|
| `SteamLibrary` | `appid` | `steam://rungameid/<appid>` |
| `EpicLibrary` | `appName` | `com.epicgames.launcher://…` |
| `GogLibrary` | `id` | a direct exe |
| `BattleNetLibrary` | `code` — **may be empty** | `battlenet://<code>`, else a guessed exe |

`SteamLibrary` alone also has a *networked* owned-library helper (user-supplied Web API key + SteamID,
TTL-cached) that surfaces owned-but-not-installed titles.

Separately, `PcGameStore` remembers where a **downloaded** PC game landed — keyed by the addon item id
— so re-opening launches the installed game instead of re-running its installer.

Each library gets its own folder via its own builder in `browse::SyntheticCatalogs`. The plan had been
to add a folder per launcher indefinitely.

Nothing bridges these. "Hades on Steam" and "Hades downloaded from a file provider" are two unrelated
entries with unrelated ids.

## The precedent we are following

Video already solves the one-item-many-sources problem, and its pieces are pure and probe-tested:
`StremioTranslate::sortCandidates`, `mergeCandidates`, `describe` and `pickAuto`, rendered through a
`NavMenu` with an explicit "Choose source…". This design deliberately mirrors that rather than
inventing a second idiom.

## 1. `PcGameId` — identity

New `native/src/core/PcGameId.{h,cpp}`. Qt-Core only, so a headless probe links it lean.

```
QString normalizeTitle(const QString& raw);          // the matching key
bool    sameGame(const QString& a, const QString& b); // both normalised
```

`normalizeTitle` lowercases, collapses whitespace, and strips punctuation, trademark symbols, and
edition/remaster noise (`Game of the Year Edition`, `Definitive Edition`, `Remastered`, `Director's
Cut`, a trailing year).

**What it must NOT strip is the point.** Sequel numerals — Arabic and Roman — are *significant*.
Merging `Hades` with `Hades II`, or `Portal` with `Portal 2`, is worse than never merging at all: the
user loses a game from their library rather than merely seeing it twice. The probe pins this
explicitly, with real pairs in both directions.

**IGDB wins when present.** The app already aggregates IGDB metadata; wherever both candidates carry a
resolved IGDB id, that decides, and the title heuristic is not consulted. Title matching is the
fallback for the (common) case where no id has been resolved yet.

**A user override store** settles the rest: "these are the same game" and "these are not". It is the
escape hatch that makes a fuzzy heuristic acceptable to ship — without it, a wrong merge has no cure.

## 2. `PcGameSource` — one candidate

Mirrors `StreamCandidate`.

```
struct PcGameSource {
    enum Kind { LauncherInstalled, LauncherOwned, Downloaded, AddonAvailable };
    Kind    kind;
    QString launcher;     // "steam" | "epic" | "gog" | "battlenet"; empty for an addon source
    QString launchId;     // appid / appName / gog id / battle.net code
    QString exePath;      // when the launch is a direct exe
    QString launchUrl;    // when the launch is a protocol URL
    QString addonItemId;  // Downloaded / AddonAvailable
    QString label;        // what the picker row shows
    bool    ready;        // launches now, with no download
};
```

`ready` is the distinction the whole feature turns on:

| Kind | Ready |
|---|---|
| `LauncherInstalled` | yes |
| `Downloaded` (PcGameStore has a resolved exe) | yes |
| `LauncherOwned` (owned, not installed) | no |
| `AddonAvailable` (offered, not downloaded) | no |

**Battle.net titles with an empty `code`** have no protocol launch and fall back to a best-effort exe
under `installDir`. They are the least reliable source kind and are labelled as such in the picker
rather than presented at parity — an honest "may not launch" beats a row that silently does nothing.

## 3. `pickAutoSource` — the launch decision

```
int pickAutoSource(const QVector<PcGameSource>& all);   // index, or -1 meaning "ask"
```

- Exactly one `ready` source → launch it.
- Several `ready` → ask.
- No `ready` source → ask.

**Play must never silently start a multi-gigabyte download.** That is the rule; everything else is
detail. It also mirrors video's preference for an instant source over a cold torrent.

Pure, so the probe pins the table including the empty-list and all-unready cases.

## 4. `browse::pcGamesCatalog` — the merged folder

Added to `native/src/browse/SyntheticCatalogs.{h,cpp}`, following the existing builders exactly: plain
lists in, a `MediaCatalog` out, no UI or store-singleton dependency.

It takes the four launcher lists plus the `PcGameStore` entries, groups by `PcGameId`, and emits **one
`MediaItem` per game** carrying its sources. The four per-launcher builders and their folders retire.

A filter inside the folder can still narrow to a single launcher, so "show me what I own on Steam"
survives without a separate folder.

## 5. Migration — the dangerous part

Today's item ids are `steam:<appid>`, `epic:<appName>`, `gog:<id>`, `bnet:<code>`. **Those ids are not
merely identity — the favourites row reconstructs each game's launch from the id prefix**
(`SyntheticCatalogs.cpp:184`), and `gog:`/`bnet:` additionally carry the resolved exe in the entry's
`path`.

Meanwhile favourites, hidden/completion marks, tags, play statistics and resume positions are all
keyed on item id.

So merging entries changes identity, and a naive change silently empties a user's favourites and loses
their play history.

The design:

- The **merged item id is the identity**; sources carry the launch information. Launching a favourite
  goes through the same source picker as launching from the folder.
- A **one-time remap** rewrites every per-item store from the old per-launcher id to the merged id:
  favourites, marks, tags, stats, playstats, resume.
- It is expressed as a **pure function** — old-id → new-id given the merged grouping — so a probe pins
  the table rather than the side effects, and mutation-tests it.
- It must be **idempotent** and must **never drop an entry it cannot map**: an unmappable old id keeps
  its existing record untouched rather than being deleted. Losing a favourite is the failure this
  section exists to prevent.

`PcGameStore` itself is unchanged — it stays keyed by addon item id, which is a *source* identity, not
a game identity.

## 6. Testing

`probe_pcgames`, sentinel `PCGAMES-OK`, registered in all three required places (its `add_executable`,
`run-headless-probes.sh`, and the `--target` list in `ci.yml`).

- `normalizeTitle` — edition/remaster/year/punctuation stripping, and the **sequel-numeral pairs that
  must not merge** in both Arabic and Roman forms.
- `sameGame` — IGDB id decides when both sides have one; title fallback otherwise; the override store
  wins over both.
- `pickAutoSource` — one ready, several ready, none ready, empty list.
- `pcGamesCatalog` — grouping across all four launchers plus a downloaded copy; a game present in only
  one place still yields exactly one item; source ordering is deterministic.
- The migration table — every old prefix maps, an unmappable id is preserved untouched, and running it
  twice equals running it once.

Every assertion mutation-tested: break the implementation, confirm the probe fails, revert, confirm
green. An assertion that passes under a broken implementation is not coverage.

Live verification through the `EB_UITEST` harness on a throwaway copy: the merged folder lists each
game once; a game with one ready source launches directly; a game with several shows the picker; a
game with no ready source shows the picker rather than downloading; and a favourite made before the
change still opens after it.

## Deliberately not in scope

- **Installing an owned-but-not-installed game.** `LauncherOwned` sources appear in the picker and are
  labelled, but handing off to `steam://install/` starts a long-running external process with its own
  progress and failure surface. Its own slice.
- **Non-Steam owned-library fetches.** Only Steam has a networked owned-list today; Epic/GOG/Battle.net
  contribute installed titles only.
- **Console/emulated games.** This is the PC folder; the ROM path is untouched.
- **Automatic IGDB resolution for the whole library.** Identity uses an IGDB id when metadata has
  already resolved one. Bulk resolution is the same follow-up the local video library is waiting on.
