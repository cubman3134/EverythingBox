# First-Run Onboarding: Google Drive Restore — Design

**Date:** 2026-07-23
**Status:** COMPLETE (T1–T3, 2026-07-23). Desktop code-verified through the entire flow up to the
credential boundary: the choice screen, the `signInAvailable` gate, the real OAuth sign-in launch, and
the signed-in `checkStatus`→pull→route chain. The full restore round-trip (real Google consent → Drive
pull → restored-profiles picker → populated library) awaits the user's one-time interactive Google
consent, which also re-mints the currently-expired refresh token — the agent cannot enter credentials.
Android "Restore" correctly declines pending the separate OAuth follow-up (a documented non-goal). The
T2 findFile data-safety window (a Drive file-query network failure being read as "empty cloud" and
seeding fresh over the real backup) is CLOSED in T3 (`Status::listReached` + pure `restorePullStage`,
pinned in `probe_onboarding`) — closed at BOTH transport layers: `findFile`'s file-query AND
`ensureFolder`'s folder list-GET now carry the same `reply->error()` guard, so a transient error mints no
duplicate empty folder and can never launder into a "proven-empty" Seed on any sync path.
**Origin:** User request — "change first startup to set up your Google Drive info instead
of creating a profile, so you can potentially use your profiles from Google Drive."

## Depends on

The multi-device sync track (`sync/multidevice`, T1–T5) — this consumes the bundle's
now-synced `profiles/list` + the CloudMerge restore. Must merge first. The Android/TV
OAuth follow-up (the recorded §5 limitation) upgrades the TV path; desktop gets the full
experience day one.

## Decisions (user-set)

- **First screen = a choice:** "Restore from Google Drive" vs "Set up a new library."
  No one is forced down the Drive path.
- **Restore = pull, then pick a profile:** sign-in → pull the cloud bundle + merge doc →
  present the restored profiles to choose from ("Add another profile" available).

## Design

### The onboarding fork (new first-run entry)

Today first-run goes straight to the create-a-profile flow (the OSK name/icon picker
validated on the TV). New: a **first screen** presented ONLY when no local profiles exist
AND onboarding hasn't completed. Two Nav-Contract actions:

- **Restore from Google Drive** → the OAuth sign-in (CloudSync::signIn — the existing
  loopback+PKCE desktop flow) → on success, pull the bundle + merge doc → if
  `profiles/list` is non-empty, show the restored profiles as a picker (the existing
  ProfileDialog/themed picker, in "choose" mode) → selecting one lands in that library
  fully populated (marks/playlists/stats/resume all restored via the sync merge).
- **Set up a new library** → the existing create-a-profile onboarding, byte-for-byte
  unchanged; this device becomes the first to seed the cloud on its next push.

### The graceful-fallback spine (load-bearing)

Every failure returns to the choice screen or the fresh path — never a dead end:

| Situation | Behavior |
|---|---|
| Sign-in cancelled / OAuth fails | back to the choice screen (with a one-line "Sign-in didn't complete") |
| Sign-in OK, `profiles/list` empty | "Nothing to restore yet" → straight into create-a-profile (seeds the cloud) |
| Sign-in OK, pull/network fails after auth | "Couldn't reach Drive" → choice screen; the refresh token is kept (retry later) |
| **Android/TV (OAuth unsolved)** | the Restore button IS shown but, on tap, reports "Drive sign-in isn't available on this device yet" and routes to fresh setup. When the Android OAuth follow-up lands, the same button just works — no onboarding change needed then. |
| Expired/revoked token (the user's current state) | the SAME sign-in flow is the recovery: re-consent mints a fresh refresh token. So this onboarding also fixes the dead-token recovery path. |

### Platform gate for the Android decline

A single predicate `CloudSync::signInAvailable()` — true on desktop, false on
`Q_OS_ANDROID` until the OAuth follow-up flips it. The Restore action stays VISIBLE on all
platforms (discoverability) but consults this on tap; false → the decline message + fresh
routing. (Alternative considered and rejected: hiding the button on Android — makes the
feature invisible and the eventual OAuth landing needs an onboarding edit; the
visible-but-declines shape is forward-compatible.)

### What "restore" actually pulls

The sync track already made this work: the bundle carries `profiles/list` (+ addon
configs, API keys — per-user) and the merge doc carries the per-item stores. Restore is
just: signIn → cloudPull (bundle apply, now carve-out-filtered) → pullAndMergeProgress →
read `profiles/list` → present. NO new sync mechanics — this is a UI/flow layer over the
shipped transport. Device-local keys (display/mode, roms/folder…) are NOT pulled (the
carve-out), so a restored TV keeps its TV mode.

## Verification

- Probe (extend probe_navqml or a lean flow-probe): the onboarding router's decision
  table as pure logic — no-local-profiles + not-onboarded → choice screen; restore +
  empty-remote → fresh; restore + populated-remote → picker; signInAvailable() false →
  decline path. (The OAuth network itself is not probe-testable — flow logic is.)
- Live (desktop): USER ACTION REQUIRED — the restore path needs a real interactive
  Google sign-in (the agent cannot enter credentials). Flow: fresh data dir → first
  screen appears → the user signs in when prompted (this also RE-AUTHENTICATES the
  currently-expired token) → profiles restore + picker → land populated. Then the fresh
  path on another clean data dir. On the TV: the Restore button shows and correctly
  declines to fresh setup.
- Suite + perf gates (onboarding is off the steady-state hot paths).

## Non-goals

- Solving Android OAuth (separate follow-up; this is forward-compatible with it).
- Account switching mid-session (Settings → Cloud Sync already handles sign-out/in).
- Merging a fresh-local library INTO a restored one (fresh vs restore is a fork at
  first-run; a later "import local into cloud" is out of scope).
- Any change to the sync/merge transport itself.
