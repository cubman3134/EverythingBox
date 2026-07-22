# VLC-Class Player Controls: Sync Offsets + Boost/Tracks Confirmation — Design

**Date:** 2026-07-21
**Status:** Complete: sync controls live (per-file + global), boost + track selection verified as features (2026-07-21).
**Origin:** User request — "VLC lets you change the sound to 200%, shows all available
subtitles, and makes the sound/subtitles sync faster or slower. Make sure we have all
those features."

## What already exists (confirmed by survey; verify-as-features in the plan)

- **Volume boost to 200%**: the player volume slider is 0..200 (MainWindow.cpp ~:618,
  `volume_->setRange(0, 200)`) over mpv `volume-max=200` software amplification
  (MpvWidget.cpp ~:50). VLC parity already present.
- **Audio + subtitle track selection**: the player's overlay card (`showSubtitleMenu`,
  MainWindow.cpp ~:5392 — a keyboard/controller-navigable scrim+card over the player,
  NOT a QMenu) lists AUDIO tracks and SUBTITLES (Off + each, checkmarks) from mpv's
  `track-list` (MpvWidget `audioTracks()` / `tracksOfType("sub")`). VLC parity present;
  mpv's `sub-auto` sidecar loading covers external .srt discovery.
- **Subtitle delay plumbing**: `MpvWidget::subDelay()/setSubDelay(seconds)` exist
  (MpvWidget.cpp ~:481-487) with ZERO UI callers.

**Verification (2026-07-21):** volume boost to 200 and multi-track (audio+subtitle)
switching were live-verified this date on the deployed Release build (state.volume=200;
audio→JPN switch with ENG subtitle rendering confirmed) — see sync-task-2-report evidence.

## What's missing (the build)

### 1. Audio-delay plumbing

`MpvWidget::audioDelay()/setAudioDelay(double seconds)` wrapping mpv `audio-delay`,
byte-parallel to the sub-delay pair.

### 2. SyncOffsets store (Settings-backed; user decision: per-file + global default)

- Per-file: `sync/<resumeKey>/audio` and `sync/<resumeKey>/sub` (double seconds).
- Global defaults: `sync/globalAudio`, `sync/globalSub` (double, default 0.0).
- Resolution at playback start (after mpv loads the file): per-file value if present,
  else the global default; applied to both mpv properties.
- Any in-player adjustment live-applies to mpv AND persists the per-file value.
- No stable resume key (some streams) → global-only applied, no per-file write.
- API shape: a small namespace/class (`SyncOffsets`) with
  `resolve(key) -> {audio, sub}`, `savePerFile(key, which, secs)`,
  `globalDefault(which)` / `setGlobalDefault(which, secs)`, `clearPerFile(key, which)`.

### 3. UI — extend the existing tracks overlay card

Beneath the AUDIO and SUBTITLES sections, two new sections:

- **AUDIO SYNC** and **SUBTITLE SYNC**, each: a live readout row ("+150 ms" / "0 ms";
  negative = audio/subs earlier, mpv sign convention surfaced in VLC terms), a `−50 ms`
  and a `+50 ms` row (each press steps 0.05 s, live-applied + per-file persisted;
  button auto-repeat for press-and-hold), **Reset** (back to the global default, clears
  the per-file entry), **Save as default** (copies the current offset to the global).
- Same rowButton/navigation mechanics the card already uses (subLeftCol_ focus chain);
  sizes through the Phase-1 `FormFactor::hitClamp` so touch works.
- No new hotkeys, no Esc-menu changes, no new surfaces.

## Error handling

- mpv property set failures are silent no-ops (consistent with the widget's style);
  the readout reflects the actual property value re-read after set.
- Corrupt/absent stored values parse to 0.0. Offsets clamped to ±10 s (VLC-range sanity).

## Verification

- Probe (`probe_playback` extension or a lean new section wherever MpvWidget-free logic
  lands): SyncOffsets semantics — per-file beats global, global applies when no
  per-file, reset clears per-file (global then applies), save-as-default copies,
  no-key → global-only, clamp.
- Live: play a library video → adjust audio sync + sub sync from the card (mpv
  properties verified via the uitest state or the readout), replay the SAME file →
  offsets restored; play a DIFFERENT file → global default applies; Reset + Save-as-
  default flows; volume slider to 200% (audible boost is not machine-checkable —
  verify the property hits 200); track card lists + switches audio/sub tracks on a
  multi-track file (use/obtain one; note which).
- Desktop suite + perf gates unchanged.

## Non-goals

- Playback speed control, equalizer, video filters (not requested).
- Themed-QML re-hosting of the overlay card (it is already Nav-Contract-navigable and
  was sanctioned through the B2 walk; a future theme pass may absorb it).
- Per-track (rather than per-file) offset memory.
