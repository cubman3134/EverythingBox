# External Video Player Handoff (Stremio-style) — Design

**Date:** 2026-07-22
**Status:** Complete: desktop verified (setting, one-off both directions, restricted gating, failure fallback); Android Intent code in place, device-unverified (rides the pending TV/emulator session).
**Origin:** User request — "handling for external video player like in stremio, etc."

## Decisions (user-set via scoping questions)

- **Setting + per-play menu**: a default-player setting AND a one-off per-play action.
- **Platforms: both** — Windows desktop (launch player exe) and Android/Android TV
  (ACTION_VIEW Intent → any installed player app claims it).

## Design

### Setting

- Themed General panel Choice row **"Video player"**: `Built-in | VLC | MPC-HC | Custom…`
  — VLC/MPC-HC options appear ONLY when detected (standard install paths + registry
  probe at panel build); `Custom…` opens the OSK for an exe path (plus an Action row
  using the native file dialog, per convention). Android variant of the row:
  `Built-in | Ask another app`.
- Stored: `player/external` = `builtin | vlc | mpc | custom | android-intent`
  (default `builtin`), `player/externalPath` (custom exe). Settings-accessor pattern.

### Per-play

- The detail play flow gains **"Open in external player…"** (shown only when a player
  is configured/detected). One-off — never changes the default.
- When the default IS external, normal Play routes there and the alternative action
  **"Play with built-in player"** appears — never trapped either way.
- **Restricted (kids) profiles: external handoff hidden entirely** (content leaving
  the app bypasses parental machinery).

### Launch

- Desktop: `QProcess::startDetached(playerExe, {urlOrPath})` — local files and
  resolved stream URLs alike. Detection helper `ExternalPlayer::detect()` returns the
  available players + paths (VLC: registry `HKLM\SOFTWARE\VideoLAN\VLC` + Program
  Files probe; MPC-HC: its standard locations).
- Android: `ACTION_VIEW` Intent, URI + `video/*` MIME via QJniObject (the resolveAuto
  pattern), `Q_OS_ANDROID`-gated; no chooser-forcing (the OS default/chooser behavior
  stands).
- Module: `native/src/core/ExternalPlayer.{h,cpp}` (detection + launch, QtCore +
  the platform bits; QProcess desktop, QJniObject Android).

### Limitations (by design, stated in-UI where cheap)

- Fire-and-forget: NO resume-position sync back, no sync-offset application, no
  completion tracking from the external session. The item still lands in Recents as
  opened (the existing RecentStore::add on the open path — keep it).

## Verification

- Probe (`probe_extplayer` or a probe_sync-style lean section): detection parsing
  (fake registry/paths via injectable roots — detection must be seam-testable),
  setting round-trip, the routing decision table (default builtin + action → external;
  default external + alternative action → builtin; restricted profile → no external
  ever). NO probe launches a real player.
- Live (desktop): with VLC or MPC-HC if installed — else the Custom path pointed at a
  harmless stub exe that logs its argv (build one in the scratchpad): set default →
  Play routes out (stub logs the URL); per-play action one-off; restricted profile
  hides it; built-in alternative works. Byte-identical ini restore.
- Android: emulator — Intent fires (logcat shows the ACTION_VIEW; the emulator may
  have no player app — the chooser/ActivityNotFound handling must be graceful: catch +
  notify "No app can play this").
- Suite + perf gates unchanged.

## Non-goals

- Playlist/channel handoff (channels are built-in-only — chaining needs our player);
  external AUDIO/reader handoff; per-player argument templates (future if asked);
  watching the external process to mark completion.
