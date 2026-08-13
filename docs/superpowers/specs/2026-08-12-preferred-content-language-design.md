# Preferred content language — one setting for subtitles, audio, manga, and torrent ranking

**Status:** approved 2026-08-12, ready for planning. Built in three layered increments.

## Problem

Language preference is scattered and partly unreachable:
- The client has a subtitle-language control, but it is buried under **Subtitles** in General
  settings (`subs/language`, ISO-639-2 like `spa`/`eng`, default empty) and only drives mpv's
  subtitle track (`slang`). There is **no** audio-language control.
- Manga language is chosen **entirely server-side** in the Allarr plugin
  (`Manga.PreferredLanguages = ["en"]`); the client cannot influence it. A manga with no English
  scanlation falls back to whatever is most complete — which is why opening one returned Spanish.
- Torrent movie/TV release + subtitle language is a **static** server config
  (`Ranking.PreferredLanguages` / `PreferredSubtitleLanguages`, language *names* like `"English"`).

Goal: one general **"Preferred content language"** setting in the client that governs all four —
subtitle track, audio track, which language Allarr pulls manga in, and the language the server
prefers when ranking torrent releases — as a **soft preference** (prefer, then fall back; nothing
is ever hidden).

## The channel — `Accept-Language`

The client sends `Accept-Language: <lang>` on its remote-addon requests; server sources read it
from `SourceContext.RequestHeaders`. This is the standard HTTP mechanism for "preferred content
language" and avoids inventing a wire path or overloading the `X-EB-Config` debrid blob (which is
addon-settings-shaped and currently has an `X-EB-Config`/`X-MMV-Config` name-drift bug on the
Allarr side).

Verified engine facts that shape the increments:
- `ForwardableHeaders(http)` forwards **every** header except a small `BlockedHeaders` set
  (`Authorization`, `Cookie`, hop-by-hop, `Host`). `Accept-Language` is **not** blocked, so it
  already reaches the `SearchAsync` and `StreamAsync` `SourceContext`s (`AddonEndpoints.cs:50,179`).
- `DetailAsync` and `MetaAsync` are called with a **bare** `new SourceContext()` — no headers
  (`AddonEndpoints.cs:73,92`). Manga's language ranking happens in `DetailAsync` (chapter listing),
  so manga needs the engine to forward headers there.

## Canonical code + mapping (shared concern)

Store one canonical form — **ISO-639-1 two-letter** (`en`, `es`) — the form `Accept-Language` and
MangaDex use natively. Each consumer maps from it:
- **mpv `slang`/`alang`** (client): emit a comma list covering 2- and 3-letter tags, e.g. `en` →
  `"en,eng"`, so mpv matches whichever tag a track carries.
- **Ranking names** (server): `en` → `"English"` (a small 2-letter → name table for the languages
  the ranker cares about).
- **MangaDex** (server): the two-letter code is used directly.

This reconciles the three formats the codebase uses today (subs `spa`, manga `en`, ranking
`"English"`). The client's existing `subs/language` value (3-letter) is migrated to the canonical
key on first read (`spa`→`es`, `eng`→`en`, empty stays "no preference").

## Increment 1 — Client: the setting, video (subs + audio), and send the header

**Client only. Independently shippable and immediately useful for video.**

- Promote the language control out of "Subtitles" into a general **"Preferred content language"**
  row in **General settings**, added to **both** settings builders — the themed
  (`ThemedPanelHost` `choice(...)`) and the classic (`QComboBox`) — mirroring the existing
  subtitle-language combo (`MainWindow.cpp` themed ~`:12729`, classic ~`:14024`). Same
  display↔code table, canonicalized to two-letter, with a first entry "Any / no preference"
  (empty).
- A single canonical accessor (e.g. `Settings::preferredLanguage()` over a canonical key), with the
  existing `subs/language` migrated into it; the old subtitle-only reads now come from the unified
  value. Keep back-compat so an existing INI is not lost.
- Drive mpv **`slang`** (as today) **and** **`alang`** (new) from the canonical value via the
  2-/3-letter mapping (`MpvWidget.cpp` where `slang` is set, ~`:421`).
- The client sets `Accept-Language: <two-letter>` on every remote-addon request (alongside the
  existing `X-EB-Config` send in `AddonManager`), so the wire is live for Increments 2–3. Empty
  preference → no header.
- **Tests:** a headless probe asserting the canonical get/set + migration (`spa`→`es`), the
  slang/alang mapping (`en`→`"en,eng"`), and that an addon request carries `Accept-Language` when
  set and omits it when empty. Both settings builders show the control (extend the existing
  settings-surface probe, per the "two builders" rule).

## Increment 2 — Manga honors the client language

**A one-line engine change + Allarr manga source. Fixes the reported case.**

- **Engine (public repo):** forward headers to `DetailAsync` — change
  `source.DetailAsync(payload, new SourceContext(), ct)` to pass
  `new SourceContext { RequestHeaders = ForwardableHeaders(http) }` (`AddonEndpoints.cs:73`). Generic
  and plugin-agnostic; names no plugin (keeps public-repo cleanliness). (Do the same for `MetaAsync`
  only if a source needs it — not required here.)
- **Allarr (private repo):** `MangaSource.DetailAsync` reads the `Accept-Language` primary language
  from `ctx.RequestHeaders`, and threads it into `MangaDexClient` so `LanguageRank` treats it as the
  top preference — **prepended** to the configured `Manga.PreferredLanguages`, not replacing it, so
  the server default remains the fallback and the existing "no match → most complete wins" behavior
  is unchanged when the language is absent.
- **Tests:** Allarr unit test — `DetailAsync` with `Accept-Language: es` ranks a Spanish chapter
  first when present, and still returns the fallback when the language is absent; with no header,
  behavior is exactly today's. Engine test — `DetailAsync` now forwards `Accept-Language` into the
  source's `SourceContext`.

## Increment 3 — Torrent movie/TV ranking honors the client language

**Engine only. The largest piece — the ranker is static-config today.**

- The ranked-search sources (`IndexerSearchSource`, `MetadataBackedVideoSource`) read the
  `Accept-Language` primary language from their `SourceContext` and thread it into
  `DefaultTorrentRanker` as a **per-request** preference that takes precedence over
  `Ranking.PreferredLanguages` / `PreferredSubtitleLanguages` (mapping two-letter → language name).
  Applied to both the audio/release-language boost (`LanguageScore`) and the subtitle boost
  (`SubtitleScore`), as an additive preference (prefer, then fall back — an absent language just
  scores 0, as today).
- The mechanism is a per-call override carried on the request or ranker call, not a mutation of the
  server's global `RankingOptions`; with no `Accept-Language`, ranking is byte-for-byte today's.
- **Tests:** engine test — a ranked search with `Accept-Language: en` boosts an English release/sub
  above an otherwise-equal foreign one; with no header, ranking is unchanged (the existing ranker
  tests stay green).

## What binds

- **`Accept-Language` is the one wire.** Set once by the client; read by manga (Inc 2) and ranking
  (Inc 3). No new header, no debrid-blob overload.
- **Soft preference, never a filter.** Every consumer *prepends* the language to its existing
  preference order; an absent language falls back exactly as today. Nothing is hidden.
- **Repo boundaries + cleanliness.** Engine changes (header forwarding, ranker per-request language)
  are generic and name no plugin — public-repo clean. Manga specifics live in Allarr (private). The
  client setting + mpv wiring + header send live in the client. This spec doc lives in the client
  repo (which already references the plugin by name).
- **Canonical two-letter + mapping** is the single source of truth; each consumer maps outward.
- **No AI attribution** in commits/PRs. Client tree is shared and has a version-bump hook — stage by
  explicit path, never `git add -A`, don't hand-edit version lines.

## Out of scope

- A multi-language ordered priority list in the UI (canonical model is a list-of-one today; the
  server sides already accept lists, so a future UI list is additive — not built now).
- Strict "this language only, else hide" filtering (rejected in favor of prefer-then-fall-back).
- Reconciling the separate `X-EB-Config`/`X-MMV-Config` debrid-header name drift (a real but
  unrelated bug; note it, fix it under its own change).
- Per-medium different fallback rules.

## Done when

- One "Preferred content language" control in General settings (both builders) drives mpv subtitle
  **and** audio track selection, migrating the old subtitle-language value.
- The client sends `Accept-Language`; opening a manga with the preference set pulls that language
  when a translation exists (and still falls back otherwise) — the reported Spanish case is fixed
  when an English chapter exists.
- Torrent movie/TV search prefers the chosen language for release audio and subtitles.
- Each increment's tests are green; the client's headless probe suite, the Allarr plugin tests, and
  the engine tests all pass; no plugin name appears in a public-repo change.
