# First-Run Drive-Restore Onboarding Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** First-run offers "Restore from Google Drive" vs "Set up a new library"; restore signs in, pulls the (now-synced) profiles + merge doc, and lets you pick a restored profile — with a graceful fallback everywhere (cancel, empty account, network fail, Android decline).

**Architecture:** A UI/flow layer over the shipped sync transport — no merge/transport changes. A choice screen injects before the startup profile picker; Restore drives `CloudSync::signIn` (async, signal-wired) → the existing pull chain → re-present the themed picker. Spec: `docs/superpowers/specs/2026-07-23-onboarding-drive-restore-design.md`.

**Tech Stack:** Qt 6.8.3, ThemedPanelHost/NavConfirm/NavMenu, the CloudSync + CloudMerge plumbing, headless probes.

## Global Constraints

- Branch: `onboarding/drive-restore` off main. Standing autonomy through the merge gate. The pre-commit hook auto-bumps the patch version — expected on every commit.
- ANCHOR ON FUNCTION NAMES. Scout anchors (main@6bd9ac5):

| Concern | Anchor |
|---|---|
| First-run condition (today: profiles-empty ONLY, no onboarded flag) | `main.cpp:213-214` (`chooseProfile = profiles.size()!=1`); inject point `MainWindow.cpp:1998` (`if (startupChooseProfile_) promptStartupProfile()`) |
| The themed picker to reuse in choose-mode | `presentProfilePicker(mustChoose)` (`MainWindow.cpp:4956`) → `presentProfileList` (`:4968`); `promptStartupProfile` (`:2035`) |
| Sign-in (async, signals) | `CloudSync::signIn` (`CloudSync.cpp:90`); completion via `signedIn(email)` / `signInFailed(error)` (CloudSync.h:80-83); wiring idiom `openCloudSync` (`MainWindow.cpp:8463-8467`, w/ themedPanelIsTop gate) |
| Pull chain (on-demand OK) | `checkStatus`+`applyRemote` (the `cloudPullAtStartup` chain, `main.cpp:68-71`) then `pullAndMergeProgress` (`MainWindow.cpp:8869`) |
| Profiles after pull | `ProfileStore::list()` (ProfileStore.h:18) — empty ⇒ fresh, populated ⇒ picker; `profiles/list` IS synced (CloudSync.h:30) |
| Device-local one-shot flag model | `display/tvPromptDone` (`Settings.cpp:182-185`, carved out at `CloudSync.cpp:398`) — the `onboarding/done` flag copies this exactly |
| Expired-token recovery = same signIn | `withAccessToken` fails→re-sign-in; `signIn` re-mints `cloud/refreshToken` (`CloudSync.cpp:169`) |

- NO transport/merge changes (that shipped). Every sign-in/pull FAILURE routes back to the choice screen or the fresh path — never a dead end.
- `signInAvailable()` new static, `Q_OS_ANDROID`-gated (false on Android until the OAuth follow-up); the Restore action stays VISIBLE everywhere, consults it on tap.
- Env recipe COMPLETE; serial builds; probes RED-first + runner/ci.yml; the create-a-profile fresh path stays BYTE-IDENTICAL (a pure prepend). The onboarding flow was TV-validated (OSK) — don't regress it.

---

### Task 1: the onboarding router + signInAvailable + choice screen

**Files:** Modify `native/src/core/CloudSync.{h,cpp}` (add `static bool signInAvailable()`), `native/src/core/Settings.{h,cpp}` (add `onboardingDone()/setOnboardingDone()` key `onboarding/done`, model on tvPromptDone), `native/src/core/CloudSync.cpp` (add `onboarding/done` to `isDeviceLocalKey` — device-local, must not sync), `native/src/ui/MainWindow.cpp/.h` (the choice screen + router at the inject point), `native/tools/probe_onboarding.cpp` (create — the router decision table as pure logic) + CMake/runner/ci.yml.

**Interfaces (Produces):** `CloudSync::signInAvailable()`; `Settings::onboardingDone/setOnboardingDone`; `MainWindow::presentOnboardingChoice()` — the entry the inject point calls; a pure `onboardingRoute(bool hasLocalProfiles, bool restorePicked, bool signInOk, bool remoteHasProfiles, bool signInAvailable) -> {ChoiceScreen|Fresh|Picker|Decline}` helper (probe-linkable).

- [ ] Probe RED (probe_onboarding): the pure `onboardingRoute` table — no-local + first-run → ChoiceScreen; restorePicked + !signInAvailable → Decline→Fresh; restore + signInOk + remote-empty → Fresh; restore + signInOk + remote-populated → Picker; restore + !signInOk → back to ChoiceScreen; signInAvailable() true desktop / false Q_OS_ANDROID (compile-guard tested via the pure param). Sentinel ONBOARDING-OK.
- [ ] Router + choice screen: at `MainWindow.cpp:1998`, when `startupChooseProfile_` AND `!Settings::onboardingDone()` AND `ProfileStore::list().isEmpty()` → `presentOnboardingChoice()` (a NavConfirm-shaped two-action themed screen: "Restore from Google Drive" / "Set up a new library"; reuse the presentProfileList panel idiom for chrome). "Set up a new library" → `Settings::setOnboardingDone(true)` + the EXISTING `promptStartupProfile()`/`presentProfilePicker(true)` fresh path unchanged. When profiles already exist OR onboardingDone → skip straight to today's `promptStartupProfile()` (no behavior change for existing users). Restore action wired in T2.
- [ ] Suite green; commit `feat: first-run onboarding router + choice screen + signInAvailable (onboarding T1)`.

---

### Task 2: the restore flow + fallback spine + live verify

**Files:** Modify `native/src/ui/MainWindow.cpp` (the Restore action handler: signInAvailable gate → signIn → the pull chain → route; the fallback handlers), `native/src/ui/MainWindow.h`.

- [ ] Restore handler: on tap, if `!CloudSync::signInAvailable()` → notify "Drive sign-in isn't available on this device yet" → route to the fresh path (setOnboardingDone + promptStartupProfile). Else `cloud_->signIn()` with `connect` to `signedIn`/`signInFailed` (the openCloudSync idiom + a topmost/onboarding-still-showing gate so a late completion after navigating away is dropped). `signInFailed` → back to the choice screen with a one-line notice (token/refresh kept). `signedIn` → run `checkStatus`+`applyRemote` (bundle apply, carve-out already filters) → on its callback `pullAndMergeProgress()` → then `ProfileStore::list()`: empty ⇒ notify "Nothing to restore yet" + fresh path (this device seeds the cloud); populated ⇒ `Settings::setOnboardingDone(true)` + `presentProfilePicker(/*mustChoose*/true)` (restored profiles render as rows). A pull/network failure AFTER auth → "Couldn't reach Drive" → choice screen (token kept).
- [ ] Suite green. **Live verify (desktop; USER ACTION for the Google consent):** fresh data-dir launch → the choice screen appears; drive "Restore" via uitest → at the sign-in step, HAND OFF to the user for the interactive Google consent (I cannot enter credentials — this same sign-in RE-MINTS the currently-expired refresh token); after consent → verify the pull runs, the restored profiles picker appears, selecting one lands in a populated library (a mark/playlist/stat from the cloud is present). Then a SECOND fresh data-dir → "Set up a new library" → the unchanged create-a-profile flow. Screenshots ob-*. Snapshot/restore any real ini touched; the throwaway data-dirs leave zero residue.
- [ ] Commit `feat: Drive restore flow + graceful fallback (onboarding T2)`.

---

### Task 3: close-out — spec status, gates, fable, merge

- [ ] Spec Status → complete (desktop verified; Android Restore correctly declines pending the OAuth follow-up). Full suite + perf 3 runs (onboarding is off the steady-state hot paths — startup adds one flag read + the pre-home choice screen only on true first-run). Fable whole-branch (dimensions: the fallback spine completeness — every failure has a non-dead-end route; the late-OAuth-completion gate; the fresh-path byte-identity/TV-OSK non-regression; onboarding/done being genuinely device-local; the restore→picker→populated-library data path). Fix rounds; merge+push+redeploy.

## Self-Review (done at write time)

- Spec coverage: choice screen ✅T1; restore=pull-then-pick ✅T2; fallback table (cancel/empty/network/Android-decline/expired-token) ✅T2; signInAvailable gate ✅T1; onboarding/done device-local ✅T1; verification (incl. the USER-action Google consent) ✅T2/T3.
- Type consistency: CloudSync::signInAvailable, Settings::onboardingDone/setOnboardingDone, MainWindow::presentOnboardingChoice, onboardingRoute — consistent across tasks.
- Ambiguities resolved: choice screen = NavConfirm-shaped two-action (scout's closest primitive); fresh path is a pure prepend (no create-flow edits); the Google consent is a user step the agent cannot perform (baked into T2's live verify) and doubles as the expired-token recovery.
