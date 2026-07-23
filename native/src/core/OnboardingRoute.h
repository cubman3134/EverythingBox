#pragma once
// The ONE pure first-run routing decision (onboarding/drive-restore T1). Header-only so it links trivially into
// both the full app (MainWindow's inject point + T2's restore flow CALL it — never duplicate the logic) and the
// headless probe_onboarding (which pins the table below). No Qt, no widgets — a total function over five bools.
//
// Inputs:
//   hasLocalProfiles  — ProfileStore::list() is non-empty on THIS device (an existing/returning user).
//   restorePicked     — the user tapped "Restore from Google Drive" on the choice screen (else: nothing picked).
//   signInOk          — a sign-in attempt succeeded (only meaningful once restore was picked + available).
//   remoteHasProfiles — after the pull, the (synced) profiles/list came down non-empty.
//   signInAvailable   — CloudSync::signInAvailable(): true on desktop, false under Q_OS_ANDROID (until OAuth).
//
// Outcome:
//   ChoiceScreen — present the two-action Restore / new-library screen (first run, nothing chosen yet; also the
//                  landing spot when a picked restore fails to sign in — never a dead end).
//   Fresh        — the existing create-a-profile path (a brand-new library; also when Restore reached an empty
//                  cloud — this device seeds it).
//   Picker       — the themed profile picker (existing local profiles, OR restored profiles pulled from Drive).
//   Decline      — Restore was picked but sign-in isn't available on this platform; the caller routes it onward
//                  to the Fresh path (kept distinct from Fresh so the caller can surface a one-line notice).

namespace mmv {

enum class OnboardingRoute { ChoiceScreen, Fresh, Picker, Decline };

inline OnboardingRoute onboardingRoute(bool hasLocalProfiles, bool restorePicked, bool signInOk,
                                       bool remoteHasProfiles, bool signInAvailable)
{
    // Existing/returning user: local profiles already exist -> straight to the picker, no choice screen.
    if (hasLocalProfiles) return OnboardingRoute::Picker;
    // First run, nothing picked yet -> offer the choice.
    if (!restorePicked) return OnboardingRoute::ChoiceScreen;
    // Restore picked but this platform can't sign in yet (Android) -> decline (caller falls through to Fresh).
    if (!signInAvailable) return OnboardingRoute::Decline;
    // Restore picked, sign-in attempted and failed -> back to the choice screen (token/refresh kept; retry).
    if (!signInOk) return OnboardingRoute::ChoiceScreen;
    // Signed in: a populated cloud -> pick a restored profile; an empty cloud -> fresh (seed the cloud here).
    return remoteHasProfiles ? OnboardingRoute::Picker : OnboardingRoute::Fresh;
}

} // namespace mmv
