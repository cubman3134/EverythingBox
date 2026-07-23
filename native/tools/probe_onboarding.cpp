// Headless check of the first-run onboarding router (onboarding/drive-restore T1). The pure decision that the
// choice screen, the inject point, and T2's restore flow all route through is mmv::onboardingRoute — a total
// function over five bools (src/core/OnboardingRoute.h). This pins its table verbatim from the plan so the UI
// layers can never drift from it, and asserts CloudSync::signInAvailable() is desktop-true / Android-false.
//
// No Qt event loop, no display, no widgets — the router is header-only and CloudSync::signInAvailable() is a
// compile-time platform gate, so this links lean (Qt6::Core for CloudSync's TU) and runs anywhere.
//
// Prints ONBOARDING-OK on success; any failure prints ONBOARDING-FAIL <cond> and exits non-zero.
#include "OnboardingRoute.h"
#include "CloudSync.h"

#include <QtGlobal>
#include <cstdio>

using mmv::OnboardingRoute;
using mmv::onboardingRoute;

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "ONBOARDING-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

int main()
{
    // The router's five inputs: onboardingRoute(hasLocalProfiles, restorePicked, signInOk, remoteHasProfiles,
    // signInAvailable). The choice screen never even shows once onboarding/done is set — that flag short-circuits
    // ahead of the router at the inject point — so the table below is the "not yet onboarded" first-run space.

    // 1. No local profiles, nothing picked yet, first run => show the choice screen.
    CHECK(onboardingRoute(/*hasLocal*/false, /*restore*/false, /*ok*/false, /*remote*/false, /*avail*/true)
          == OnboardingRoute::ChoiceScreen);

    // 2. Restore picked but sign-in isn't available on this platform (Android) => Decline (caller -> Fresh).
    CHECK(onboardingRoute(false, /*restore*/true, false, false, /*avail*/false) == OnboardingRoute::Decline);
    // Decline is independent of whether the (never-attempted) sign-in "would" have found remote profiles.
    CHECK(onboardingRoute(false, true, false, /*remote*/true, /*avail*/false) == OnboardingRoute::Decline);

    // 3. Restore picked, signed in, but the cloud is empty => Fresh (this device seeds the cloud).
    CHECK(onboardingRoute(false, /*restore*/true, /*ok*/true, /*remote*/false, /*avail*/true)
          == OnboardingRoute::Fresh);

    // 4. Restore picked, signed in, cloud populated => Picker (pick a restored profile).
    CHECK(onboardingRoute(false, /*restore*/true, /*ok*/true, /*remote*/true, /*avail*/true)
          == OnboardingRoute::Picker);

    // 5. Restore picked, sign-in FAILED => back to the choice screen (never a dead end; token/refresh kept).
    CHECK(onboardingRoute(false, /*restore*/true, /*ok*/false, /*remote*/false, /*avail*/true)
          == OnboardingRoute::ChoiceScreen);
    CHECK(onboardingRoute(false, /*restore*/true, /*ok*/false, /*remote*/true, /*avail*/true)
          == OnboardingRoute::ChoiceScreen);

    // 5b. T2 semantics: the restore flow feeds the WHOLE auth+pull leg into signInOk (finishOnboardingRestore's
    //     `restoreOk`), so a pull/network failure AFTER a successful sign-in reuses the SAME ChoiceScreen row as a
    //     sign-in failure — no new enum value. The distinction (retry the sign-in vs. retry the pull) is only in
    //     the caller's notice text; the ROUTE is identical (back to the choice screen, token kept, done=false).
    CHECK(onboardingRoute(false, /*restore*/true, /*ok(=auth&&pull)*/false, /*remote*/false, /*avail*/true)
          == OnboardingRoute::ChoiceScreen);
    //     And the post-sign-in Picker-vs-Fresh IS the router's remoteHasProfiles branch (restored rows vs. seed):
    CHECK(onboardingRoute(false, true, /*ok*/true, /*remote*/true,  true) == OnboardingRoute::Picker);
    CHECK(onboardingRoute(false, true, /*ok*/true, /*remote*/false, true) == OnboardingRoute::Fresh);

    // 6. Existing/returning user: local profiles already exist => straight to the picker, no choice screen,
    //    regardless of any restore inputs (the choice screen is first-run only).
    CHECK(onboardingRoute(/*hasLocal*/true, false, false, false, true) == OnboardingRoute::Picker);
    CHECK(onboardingRoute(/*hasLocal*/true, true,  true,  false, true) == OnboardingRoute::Picker);
    CHECK(onboardingRoute(/*hasLocal*/true, true,  false, false, false) == OnboardingRoute::Picker);

    // 7. signInAvailable(): true on desktop, false under Q_OS_ANDROID (the codebase platform-gate idiom). The
    //    compile-guard branch is exercised by the pure param above; here we pin the live static matches the
    //    build's platform so the two can't disagree.
#ifdef Q_OS_ANDROID
    CHECK(CloudSync::signInAvailable() == false);
#else
    CHECK(CloudSync::signInAvailable() == true);
#endif

    if (failures == 0) { std::puts("ONBOARDING-OK"); return 0; }
    std::fprintf(stderr, "ONBOARDING: %d check(s) failed\n", failures);
    return 1;
}
