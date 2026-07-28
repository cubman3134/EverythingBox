# Trakt read-side foundation + calendar — design

GitHub issue #23, first slice. Roadmap #8.

## Where we are

`TraktClient` is a **scrobbler and nothing else**. It runs the OAuth device-code flow, stores and
refreshes tokens, and calls `scrobbleStart` / `scrobblePause` / `scrobbleStop` with an
`imdbStreamId` — `"tt123"` for a movie, `"ttShow:season:episode"` for an episode. Since the
local-library work it scrobbles local playback too.

So the app **writes** to Trakt and never **reads** from it. Everything in #23 is the read direction.

Trakt is off by default: there is no built-in client id, so a user registers their own Trakt API app
and pastes the id and secret into Settings. All empty ⇒ Trakt is off.

## Scope of this spec

#23 bundles three features. This spec covers **the read-side foundation plus the calendar**:

- the parse layer,
- the identity mapper,
- the authorised read call,
- the calendar surface.

**Watchlist / collection** and **watched-history backfill** are deliberately deferred to their own
specs. Both depend on the identity mapper, and proving that mapper on the smallest surface first is
the reason for the split. The history backfill is also the only part that *writes* to `ItemMarks`,
and it should not ride in alongside two read-only features.

The issue itself calls the calendar the headline: "if only one part of this issue ships, it should be
this one."

## The hard part: identity

The app is **IMDB-keyed**. `MediaItem::imdbStreamId` is what the stream resolver, the subtitle
fetcher and the scrobbler all key on. Trakt returns an `ids` object per show and per episode that
*may* contain `imdb`, but may carry only `tmdb` / `tvdb` / `trakt`.

An entry with no IMDB id cannot join against anything the app stores.

**Decision: show it, marked as not playable.** A calendar that silently omits a third of your shows is
worse than one that says "airing Thursday — no source found": you still learn the episode exists. The
alternative — resolving TMDB/TVDB → IMDB through a metadata addon — buys coverage at the cost of a
network round-trip per unmatched item, a cache and a failure path, and it can still come back empty,
so the "show it anyway" state is needed regardless. That resolution is a good later addition on top
of this design, not a prerequisite.

This costs almost nothing to implement, because `MediaItem` already has the right shape: an entry
with an IMDB id gets `imdbStreamId` set and becomes playable through the existing resolve path; an
entry without simply leaves it empty, which is already "no source".

## 1. `TraktRead` — pure parsing

New `native/src/core/TraktRead.{h,cpp}`. Qt-Core only (QJson*, QString, QDateTime), no network and
no GUI, so a headless probe links it lean.

```
struct TraktIds { QString imdb, tmdb, tvdb, trakt; };

struct CalendarEntry {
    QDateTime airsAtUtc;
    QString   showTitle;
    TraktIds  showIds;
    int       season = 0;
    int       episode = 0;
    QString   episodeTitle;
    TraktIds  episodeIds;
    QString   posterUrl;      // "" when Trakt gave none
};

QVector<CalendarEntry> parseMyShowsCalendar(const QByteArray& json);
```

The parser is **total and tolerant**: a malformed entry is skipped rather than aborting the batch, a
missing `ids` sub-object yields an empty `TraktIds`, and an unparseable date drops that entry. One bad
row must never cost the user their whole calendar.

The exact Trakt response field names must be confirmed against Trakt's API docs during
implementation. Whatever they are, the parser's tolerance requirements above do not change, and the
probe fixtures are written from the real observed response, not from assumption.

## 2. `TraktIds` mapping — the unit the follow-ups reuse

In the same file, pure:

```
// "ttShow:season:episode" when the SHOW has an imdb id; "" otherwise.
QString imdbStreamIdFor(const TraktIds& showIds, int season, int episode);
```

Note it keys on the **show's** IMDB id, not the episode's — that is the form the rest of the app
already uses (`ttShow:season:episode`), and it is what the scrobbler emits today.

An empty return is the documented signal for "not playable", not an error.

This is the piece watchlist/collection and the history backfill will both build on, which is why it is
carved out and probe-pinned now rather than inlined into the calendar builder.

## 3. `TraktClient` gains its read half

```
void fetchMyShowsCalendar(int daysBack, int daysForward,
                          std::function<void(bool ok, QVector<CalendarEntry>)> cb);
```

- Routed through the **same `ensureValidToken` gate** the scrobbler uses. Token refresh, expiry and
  the rotated-refresh-token handling stay in exactly one place; no read path may issue a raw request.
- Returns immediately with `ok=false` and an empty list when Trakt is not configured or not
  connected. No network call, no error surfaced — Trakt being off is not a failure.
- Rate limits are Trakt's; the fetch cadence below is what keeps us inside them.

## 4. `browse::traktCalendarCatalog` — a pure builder

Added to `native/src/browse/SyntheticCatalogs.{h,cpp}`, following the existing pattern exactly
(`recentsCatalog`, `favoritesCatalog`, `playlistsCatalog` …): a plain list in, a `MediaCatalog` out,
no UI or store-singleton dependency.

```
MediaCatalog traktCalendarCatalog(const QVector<CalendarEntry>& entries, const QDateTime& nowUtc);
```

- Sorted by air time, soonest first.
- Each item: `title` = show title, `subtitle` = `S01E04 · Thursday` (or "no source found" when
  unplayable), `type` = `"episode"`, `thumbnailUrl` = the poster when Trakt gave one.
- `imdbStreamId` set from `imdbStreamIdFor(...)`. When it comes back empty the item is left with no
  `url` and no `imdbStreamId` — already the app's representation of "nothing to play" — and the
  subtitle says so.
- Entries whose air time is in the past relative to `nowUtc` are excluded here. Recently-aired items
  are #25's job ("You missed"), and having two surfaces both claim the same episode would be worse
  than either alone.

## 5. Surfacing

A home shelf plus a browse destination, both rendered from that catalog through the existing
synthetic-catalog path — no new rendering code.

**When Trakt is not configured or not connected, neither exists.** No row, no placeholder, no
"connect Trakt" hint. This matches how the existing Trakt settings rows already degrade, and it keeps
the home screen of a user who has never heard of Trakt exactly as it is today. Advertising a feature
whose setup requires registering your own developer API app is a poor trade.

## 6. Fetching and caching

- Fetched at startup behind a debounce, and refreshed on a cadence — not per navigation.
- Cached to disk so an offline launch still shows the last known calendar. A stale calendar is far
  more useful than an empty one.
- **The cache is device-level, not per-profile.** The Trakt token is app-wide (`trakt/access`), so
  every profile on the device sees the same account's calendar. Stating this because every other
  media surface in the app *is* per-profile, and a reader will assume this one is too.
- **The cache keys must be excluded from `SettingsTxn::inScope`.** They are background-written, and
  the settings save/discard work just merged: an in-scope key written by a background fetch produces
  a phantom dirty count and gets clobbered by Discard. This is exactly the class of bug that spec's
  §2 exists to prevent, and the same async trap that caught the Trakt *token* keys.

## 7. Testing

`probe_trakt`, sentinel `TRAKT-OK`, registered in all three required places (its `add_executable`,
`run-headless-probes.sh`, and the `--target` list in `ci.yml`).

- `parseMyShowsCalendar` against captured fixtures: a normal batch; an entry with no `imdb` id; an
  entry with no `ids` at all; a malformed entry mid-batch (the surrounding entries must survive); an
  unparseable date; empty and non-JSON input.
- `imdbStreamIdFor`: full ids, show-imdb-missing, season/episode zero and negative.
- `traktCalendarCatalog`: ordering, the playable / not-playable split, past-entry exclusion, and an
  empty input yielding an empty catalog rather than a malformed one.

Every assertion mutation-tested: break the implementation, confirm the probe fails, revert, confirm
green. An assertion that passes under a broken implementation is not coverage.

**A stated limit on verification.** End-to-end confirmation needs a real Trakt account and a
registered developer app. The parse, mapping and building are provable against fixtures; the live
authorised fetch is **not** verifiable without the user's credentials, which will not be entered on
their behalf. That leaves the same residual as the Drive-restore trigger: the code path is reasoned
and unit-covered, but unproven against the live service until the user drives it.

## Deliberately not in scope

- **Watchlist and collection rows** — their own spec, on this foundation.
- **Watched-history backfill** — its own spec; it is the only piece that writes to `ItemMarks`.
- **"You missed" / recently-aired** — issue #25, which depends on this and on the backfill.
- **TMDB/TVDB → IMDB resolution** via metadata addons. A good later addition; the "not playable"
  state this spec ships is what makes it optional rather than blocking.
- **A bundled Trakt client id.** Trakt stays user-configured.
